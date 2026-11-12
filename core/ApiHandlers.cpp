#include "ApiHandlers.h"

#include "ApiJson.h"
#include "MosaicPlanning.h"
#include "LibraryVisuals.h"
#include "PreviewEvaluation.h"
#include "HistoryStore.h"
#include "UnicodeIO.h"

#include <opencv2/imgproc.hpp>

// Unicode I/O includes the Windows spooler macro after ApiMetadata.h.
#ifdef GetJob
#undef GetJob
#endif

#include <algorithm>
#include <exception>
#include <charconv>
#include <climits>
#include <filesystem>
#include <list>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mosaicraft
{

namespace
{

ApiResponse resultResponse(int status, const ServiceResult& result)
{
    return {status, serviceResultToJson(result)};
}

nlohmann::json byteRangeJson(const ByteRange &range)
{
    return {{"low", range.low}, {"high", range.high}};
}

ApiResponse mosaicPlanResponse(const std::string &body)
{
    MosaicRequest request;
    RequestError error;
    if (!parseMosaicRequestJson(body, request, error))
    {
        return resultResponse(400, ServiceResult::failure(1, error.message, "invalid_argument", error.field));
    }
    MosaicPlan plan;
    const auto result = planMosaic(std::move(request), plan);
    if (!result.ok)
    {
        return resultResponse(400, result);
    }
    const auto &output = plan.output;
    return {200, apiOkJson("plan",
        {{"sourceWidth", plan.sourceWidth}, {"sourceHeight", plan.sourceHeight},
         {"workingWidth", plan.target.width}, {"workingHeight", plan.target.height},
         {"columns", output.tilesX}, {"rows", output.tilesY}, {"tileCount", uint64_t(output.tilesX) * output.tilesY},
         {"tileWidth", output.tileWidth}, {"tileHeight", output.tileHeight}, {"width", output.width}, {"height", output.height},
         {"format", output.format}, {"requestedFormat", output.requestedFormat}, {"path", output.path},
         {"mode", plan.mode}, {"writeMode", plan.writeMode}, {"files", plan.files}, {"rawBytes", plan.rawBytes},
         {"matchingRam", byteRangeJson(plan.matchingRam)}, {"writingRam", byteRangeJson(plan.writingRam)},
         {"matchingVram", plan.matchingVram ? byteRangeJson(*plan.matchingVram) : nlohmann::json(nullptr)},
         {"fileBytes", byteRangeJson(plan.fileBytes)}, {"warnings", plan.warnings}})};
}

ApiResponse mosaicCoverageResponse(const std::string &body)
{
    MosaicRequest request;
    RequestError error;
    if (!parseMosaicRequestJson(body, request, error))
    {
        return resultResponse(400, ServiceResult::failure(1, error.message, "invalid_argument", error.field));
    }
    // 同一服务只计算一张覆盖图，快速切换表单不会重复占用工作图内存。
    static std::mutex mutex;
    const std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        return resultResponse(400, ServiceResult::failure(1, "another coverage check is running", "library_busy"));
    }
    MosaicCoverage coverage;
    const auto result = measureMosaicCoverage(request, coverage);
    if (!result.ok)
    {
        return resultResponse(400, result);
    }
    return {200, apiOkJson("coverage",
        {{"target", colorDistributionToJson(coverage.target)}, {"library", colorDistributionToJson(coverage.library)},
         {"columns", coverage.columns}, {"rows", coverage.rows}, {"mapColumns", coverage.mapColumns},
         {"mapRows", coverage.mapRows}, {"minimumCandidates", coverage.minimumCandidates},
         {"noCandidates", coverage.noCandidates}, {"fewCandidates", coverage.fewCandidates},
         {"threshold", coverage.threshold}, {"lRange", request.config.lRange}})};
}

ApiResponse badRequest(const std::string& error)
{
    return resultResponse(400, ServiceResult::failure(1, error));
}

ApiResponse badRequest(const RequestError &error)
{
    RuntimeContext runtime(false);
    return resultResponse(
        400, runtimeResult(runtimeFailure(runtime, 1, error.message, "invalid_argument", error.field), runtime));
}

ApiResponse internalError(const std::string& error)
{
    return resultResponse(500, ServiceResult::failure(1, error));
}

int databaseMaintenanceErrorStatus(const ServiceResult& status)
{
    return status.exitCode == 2 ? 500 : 400;
}

bool hasBool(const nlohmann::json& body, const char* key)
{
    return body.contains(key) && body[key].is_boolean();
}

bool hasString(const nlohmann::json& body, const char* key)
{
    return body.contains(key) && body[key].is_string();
}

bool hasInteger(const nlohmann::json& body, const char* key)
{
    return body.contains(key) && body[key].is_number_integer();
}

bool matchesErrorShape(const nlohmann::json& body,
                       const std::string& shape,
                       const std::string& responseKey)
{
    if (!hasBool(body, "ok") || body["ok"].get<bool>()) return false;
    if (!hasString(body, "message")) return false;

    if (shape == "apiError") {
        return true;
    }
    if (shape == "serviceResult") {
        return hasInteger(body, "exitCode");
    }
    if (shape == "jobError") {
        return body.contains("job");
    }
    if (shape == "payloadResult") {
        return hasInteger(body, "exitCode") &&
               !responseKey.empty() &&
               body.contains(responseKey);
    }
    return false;
}

std::string statusText(int status)
{
    std::ostringstream oss;
    oss << status;
    return oss.str();
}

ApiResponse previewConflict(const std::string &message)
{
    return resultResponse(409, ServiceResult::failure(1, message, "preview_stale", "previewId"));
}

ApiResponse previewNotFound()
{
    return resultResponse(404, ServiceResult::failure(1, "preview is unavailable", "preview_unavailable"));
}

ApiResponse previewImageNotFound()
{
    return resultResponse(404, ServiceResult::failure(1, "preview image is unavailable", "preview_image_unavailable"));
}

std::string jpegDataUrl(const std::vector<unsigned char> &bytes)
{
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string value = "data:image/jpeg;base64,";
    value.reserve(value.size() + (bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3)
    {
        const unsigned left = bytes[i];
        const unsigned middle = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const unsigned right = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        value += alphabet[left >> 2];
        value += alphabet[((left & 3) << 4) | (middle >> 4)];
        value += i + 1 < bytes.size() ? alphabet[((middle & 15) << 2) | (right >> 6)] : '=';
        value += i + 2 < bytes.size() ? alphabet[right & 63] : '=';
    }
    return value;
}

ApiResponse previewQuality(const std::string &body, JobManager &jobs)
{
    const auto input = nlohmann::json::parse(body, nullptr, false);
    if (!input.is_object() || !hasString(input, "previewId") || !input.contains("positions") ||
        !input["positions"].is_array() || (input.contains("explain") && !input["explain"].is_boolean()))
    {
        return badRequest("quality requires a preview ID and a positions array");
    }
    const bool explain = input.value("explain", false);
    const auto &positions = input["positions"];
    if (positions.empty() || positions.size() > (explain ? 1 : 512))
    {
        return badRequest("too many positions for one quality request");
    }
    PreviewSnapshot preview;
    if (!jobs.getPreview(input["previewId"].get<std::string>(), preview) || !preview.selection)
    {
        return previewConflict("preview expired");
    }
    const auto &selection = *preview.selection;
    std::vector<PreviewProbe> probes;
    probes.reserve(positions.size());
    for (const auto &position : positions)
    {
        if (!position.is_object() || !position.contains("position") || !position["position"].is_number_unsigned() ||
            position["position"] >= selection.imageIds.size())
        {
            return badRequest("invalid tile position");
        }
        PreviewProbe probe{position["position"].get<size_t>()};
        if (position.contains("imageId"))
        {
            if (!position["imageId"].is_number_unsigned() || position["imageId"] > INT_MAX || !hasString(position, "version"))
            {
                return badRequest("invalid tile replacement");
            }
            probe.replacement = position["imageId"].get<int>();
            const auto *entry = selection.catalog ? selection.catalog->find(probe.replacement) : nullptr;
            const auto asset = entry ? previewAsset(*entry) : PreviewAsset{};
            if (!asset.available || asset.version() != position["version"].get_ref<const std::string &>())
            {
                return previewConflict("replacement image changed");
            }
        }
        probes.push_back(probe);
    }
    std::vector<PreviewTileScore> scores;
    const auto result = evaluatePreview(preview.request, selection, probes, explain, scores);
    if (!result.ok)
    {
        return resultResponse(result.runtime.diagnostic.code == "preview_stale" ? 409 : 400, result);
    }
    auto rows = nlohmann::json::array();
    for (const auto &score : scores)
    {
        nlohmann::json row{{"position", score.position}, {"imageId", score.imageId},
                           {"score", score.score ? nlohmann::json(*score.score) : nlohmann::json(nullptr)}};
        if (explain)
        {
            row["target"] = jpegDataUrl(score.targetJpeg);
            row["manual"] = probes.front().replacement != 0;
            auto &candidates = row["candidates"] = nlohmann::json::array();
            for (const auto &candidate : score.candidates)
            {
                candidates.push_back({{"imageId", candidate.imageId}, {"score", candidate.score},
                                      {"features", candidate.features}});
            }
            if (!probes.front().replacement && score.position < selection.decisions.size())
            {
                const auto &decision = selection.decisions[score.position];
                if (decision.rank && !score.candidates.empty())
                {
                    row["decision"] = {{"rank", decision.rank}, {"topN", decision.topN},
                                       {"neighborPenalty", decision.neighborPenalty},
                                       {"reusePenalty", score.candidates.front().useCount * preview.request.config.usePenalty}};
                }
            }
        }
        rows.push_back(std::move(row));
    }
    return {200, apiOkJson("quality", std::move(rows))};
}

ApiResponse submitPreview(const std::string &body, JobManager &jobs)
{
    const auto payload = nlohmann::json::parse(body, nullptr, false);
    if (!payload.is_object() || payload.size() != 2 || !payload.contains("request") ||
        !payload.contains("seed") || !payload["seed"].is_number_unsigned() ||
        payload["seed"].get<uint64_t>() > UINT32_MAX)
    {
        return badRequest("preview requires request and a uint32 seed");
    }
    MosaicRequest request;
    RequestError error;
    if (!parseMosaicRequestJson(payload["request"].dump(), request, error))
    {
        return badRequest(error);
    }
    try
    {
        const auto id = jobs.submitPreview(std::move(request), payload["seed"].get<uint32_t>());
        return {202, apiOkJson("preview", {{"id", id}, {"state", "queued"}})};
    }
    catch (const std::exception &exception)
    {
        return internalError(exception.what());
    }
}

ApiResponse getPreview(const std::string &id, JobManager &jobs)
{
    PreviewSnapshot snapshot;
    if (!jobs.getPreview(id, snapshot))
    {
        return previewNotFound();
    }
    nlohmann::json body{{"id", snapshot.id},
                        {"state", jobStateName(snapshot.state)},
                        {"seed", snapshot.seed},
                        {"progress", progressToJson(snapshot.progress)}};
    body["runtime"] = runtimeSnapshotToJson(snapshot.result.runtime).at("runtime");
    if (snapshot.state == JobState::Failed)
    {
        body["result"] = serviceResultToJson(snapshot.result);
    }
    if (snapshot.fileSample)
    {
        const auto &sample = *snapshot.fileSample;
        body["fileSample"] = {{"bytes", byteRangeJson(sample.bytes)}, {"regions", sample.regions}, {"pixels", sample.pixels}};
    }
    if (snapshot.comparison)
    {
        const auto &value = *snapshot.comparison;
        const auto score = [&](double number) { return value.compared ? nlohmann::json(number) : nlohmann::json(nullptr); };
        body["comparison"] = {{"compared", value.compared}, {"changed", value.changed},
            {"a", {{"matched", value.matchedA}, {"unique", value.uniqueA}, {"mean", score(value.meanA)},
                    {"p90", score(value.p90A)}, {"image", jpegDataUrl(value.imageA)}}},
            {"b", {{"matched", value.matchedB}, {"unique", value.uniqueB}, {"mean", score(value.meanB)},
                    {"p90", score(value.p90B)}, {"image", jpegDataUrl(value.imageB)}}},
            {"mapColumns", value.mapColumns}, {"mapRows", value.mapRows}, {"changedMap", value.changedMap}};
    }
    if (snapshot.selection)
    {
        const auto &selection = *snapshot.selection;
        body["columns"] = selection.columns;
        body["rows"] = selection.rows;
        body["featureWidth"] = selection.featureWidth;
        body["featureHeight"] = selection.featureHeight;
        body["featureBackend"] = selection.featureBackend;
        body["scoreBackend"] = selection.scoreBackend;
        body["fallbackReason"] = snapshot.result.runtime.fallbackReason;
        body["editKey"] = selection.editKey;
        body["imageIds"] = selection.imageIds;
        body["missing"] = snapshot.result.runtime.failed;
        body["metrics"] = snapshot.result.runtime.metrics;
        auto &versions = body["assetVersions"] = nlohmann::json::object();
        for (const auto &[imageId, asset] : selection.assets)
        {
            versions[std::to_string(imageId)] = asset.version();
        }
    }
    return {200, apiOkJson("preview", std::move(body))};
}

bool previewNumber(const ApiQueryParams &query, const char *key, size_t &value, bool required)
{
    const auto entry = query.find(key);
    if (entry == query.end())
    {
        return !required;
    }
    const auto &text = entry->second;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

nlohmann::json previewCatalogItem(const PreviewCatalogEntry &entry)
{
    const auto asset = previewAsset(entry);
    return {{"id", entry.id}, {"name", entry.name}, {"version", asset.version()}, {"available", asset.available}};
}

ApiResponse previewRecommendations(const ApiRequest &request, JobManager &jobs)
{
    PreviewSnapshot snapshot;
    if (!jobs.getPreview(request.id, snapshot) || !snapshot.selection || !snapshot.selection->catalog)
    {
        return previewNotFound();
    }
    size_t position = 0;
    const auto &selection = *snapshot.selection;
    if (!previewNumber(request.query, "position", position, true) || position >= selection.indices.size())
    {
        return badRequest("invalid tile position");
    }
    auto images = nlohmann::json::array();
    for (int id : selection.recommendationsAt(position))
    {
        if (const auto *entry = selection.catalog->find(id))
        {
            images.push_back(previewCatalogItem(*entry));
        }
    }
    return {200, apiOkJson("recommendations", {{"position", position}, {"images", std::move(images)}})};
}

std::string lowerAscii(std::string value)
{
    for (auto &ch : value)
    {
        if (ch >= 'A' && ch <= 'Z')
        {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

ApiResponse browsePreviewLibrary(const ApiRequest &request, JobManager &jobs)
{
    PreviewSnapshot snapshot;
    if (!jobs.getPreview(request.id, snapshot) || !snapshot.selection || !snapshot.selection->catalog)
    {
        return previewNotFound();
    }
    size_t offset = 0;
    const auto query = request.query.find("q");
    const auto filter = query == request.query.end() ? std::string{} : lowerAscii(query->second);
    if (!previewNumber(request.query, "offset", offset, false) || filter.size() > 256)
    {
        return badRequest("invalid library query");
    }
    constexpr size_t pageSize = 24;
    auto images = nlohmann::json::array();
    const auto &entries = snapshot.selection->catalog->entries;
    size_t total = 0;
    if (filter.empty())
    {
        total = entries.size();
        // 无筛选时直接定位页面，避免大图库跳页时遍历此前的所有素材。
        if (offset < total)
        {
            const size_t end = offset + std::min(pageSize, total - offset);
            for (size_t i = offset; i < end; ++i)
            {
                images.push_back(previewCatalogItem(entries[i]));
            }
        }
    }
    else
    {
        for (const auto &entry : entries)
        {
            if (std::to_string(entry.id) != filter && lowerAscii(entry.name).find(filter) == std::string::npos)
            {
                continue;
            }
            if (total >= offset && images.size() < pageSize)
            {
                images.push_back(previewCatalogItem(entry));
            }
            ++total;
        }
    }
    const bool more = offset < total && total - offset > images.size();
    return {200, apiOkJson("library", {{"images", std::move(images)}, {"offset", offset}, {"more", more}, {"total", total}})};
}

class PreviewThumbnailCache
{
    using Entry = std::pair<std::string, std::string>;
    std::mutex m_mutex;
    std::list<Entry> m_recent;
    std::unordered_map<std::string, std::list<Entry>::iterator> m_entries;
    size_t m_bytes = 0;
    static constexpr size_t budget = 32ULL * 1024 * 1024;

  public:
    std::string get(const std::string &key)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto entry = m_entries.find(key);
        if (entry == m_entries.end())
        {
            return {};
        }
        m_recent.splice(m_recent.begin(), m_recent, entry->second);
        return entry->second->second;
    }

    void put(const std::string &key, const std::string &jpeg)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const size_t bytes = key.size() * 2 + jpeg.size();
        if (jpeg.empty() || bytes > budget || m_entries.find(key) != m_entries.end())
        {
            return;
        }
        m_recent.emplace_front(key, jpeg);
        try
        {
            m_entries.emplace(key, m_recent.begin());
        }
        catch (...)
        {
            m_recent.pop_front();
            throw;
        }
        m_bytes += bytes;
        while (m_bytes > budget || m_entries.size() > 4096)
        {
            const auto &old = m_recent.back();
            m_bytes -= old.first.size() * 2 + old.second.size();
            m_entries.erase(old.first);
            m_recent.pop_back();
        }
    }
};

ApiResponse thumbnailResponse(const PreviewAsset &asset, int edge)
{
    if (!asset.available)
    {
        return previewImageNotFound();
    }
    if (!asset.current())
    {
        return previewConflict("selected image changed; refresh the preview");
    }
    try
    {
        // 缓存跨预览共享，仍需先通过当前预览的素材访问和版本检查。
        static PreviewThumbnailCache thumbnails;
        const std::string cacheKey = asset.path + '\0' + asset.version() + ':' + std::to_string(edge);
        std::string cached = thumbnails.get(cacheKey);
        if (!cached.empty())
        {
            return {200, apiOkJson(), {}, std::move(cached)};
        }
        const auto image = imreadUnicode(asset.path, cv::IMREAD_COLOR);
        if (image.empty())
        {
            return previewImageNotFound();
        }
        if (!asset.current())
        {
            return previewConflict("selected image changed; refresh the preview");
        }
        const double scale = std::min(1.0, static_cast<double>(edge) / std::max(image.cols, image.rows));
        cv::Mat thumbnail;
        cv::resize(image, thumbnail, {std::max(1, cvRound(image.cols * scale)),
                                      std::max(1, cvRound(image.rows * scale))}, 0, 0, cv::INTER_AREA);
        std::vector<unsigned char> jpeg;
        cv::imencode(".jpg", thumbnail, jpeg, {cv::IMWRITE_JPEG_QUALITY, 80});
        if (jpeg.empty())
        {
            return previewImageNotFound();
        }
        std::string bytes(jpeg.begin(), jpeg.end());
        thumbnails.put(cacheKey, bytes);
        return {200, apiOkJson(), {}, std::move(bytes)};
    }
    catch (const std::exception &error)
    {
        return internalError(error.what());
    }
}

ApiResponse previewImage(const ApiRequest &request, JobManager &jobs)
{
    PreviewSnapshot snapshot;
    if (!jobs.getPreview(request.id, snapshot) || !snapshot.selection)
    {
        return previewNotFound();
    }
    const auto jpegResponse = [](const std::vector<unsigned char> &jpeg) -> ApiResponse
    {
        if (jpeg.empty())
        {
            return previewImageNotFound();
        }
        return {200, apiOkJson(), {}, std::string(jpeg.begin(), jpeg.end())};
    };
    if (request.artifactId == "reference")
    {
        return jpegResponse(snapshot.selection->referenceJpeg);
    }
    int imageId = 0, edge = 0;
    const auto &key = request.artifactId;
    const auto dash = key.find('-');
    if (dash == std::string::npos)
    {
        return badRequest("thumbnail must identify an image and a size");
    }
    const auto idResult = std::from_chars(key.data(), key.data() + dash, imageId);
    const auto sizeResult = std::from_chars(key.data() + dash + 1, key.data() + key.size(), edge);
    if (idResult.ec != std::errc{} || idResult.ptr != key.data() + dash ||
        sizeResult.ec != std::errc{} || sizeResult.ptr != key.data() + key.size() ||
        (edge != 16 && edge != 32 && edge != 64 && edge != 128))
    {
        return badRequest("invalid thumbnail image ID or size");
    }
    const auto selected = snapshot.selection->assets.find(imageId);
    PreviewAsset asset;
    if (selected != snapshot.selection->assets.end())
    {
        asset = selected->second;
    }
    else if (snapshot.selection->catalog)
    {
        if (const auto *entry = snapshot.selection->catalog->find(imageId))
        {
            asset = previewAsset(*entry);
        }
    }
    return thumbnailResponse(asset, edge);
}

struct BrowserStore
{
    using Clock = std::chrono::steady_clock;
    std::mutex mutex;
    uint64_t next = 1;
    std::list<std::pair<Clock::time_point, std::shared_ptr<LibraryBrowser>>> recent;

    std::shared_ptr<const LibraryBrowser> get(const std::string &id)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto now = Clock::now();
        recent.remove_if([&](const auto &item) { return now - item.first > std::chrono::minutes(10); });
        const auto found = std::find_if(recent.begin(), recent.end(), [&](const auto &item) { return item.second->id == id; });
        if (found == recent.end())
        {
            return {};
        }
        found->first = now;
        recent.splice(recent.begin(), recent, found);
        return found->second;
    }

    void add(const std::shared_ptr<LibraryBrowser> &browser)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        browser->id = "catalog-" + std::to_string(next++);
        recent.emplace_front(Clock::now(), browser);
        if (recent.size() > 4)
        {
            recent.pop_back();
        }
    }
};

BrowserStore &browserStore()
{
    static BrowserStore store;
    return store;
}

ApiResponse catalogPage(const LibraryBrowser &browser, size_t offset)
{
    auto items = nlohmann::json::array();
    const auto total = browser.ordered.size();
    const auto count = offset < total ? std::min<size_t>(24, total - offset) : 0;
    for (size_t i = 0; i < count; ++i)
    {
        const auto &entry = browser.ordered[offset + i];
        auto item = previewCatalogItem(entry);
        const auto tiles = browser.tiles.find(entry.id);
        const auto distance = browser.distances.find(entry.id);
        if (tiles != browser.tiles.end())
        {
            item["tiles"] = tiles->second;
        }
        if (distance != browser.distances.end())
        {
            item["distance"] = distance->second;
        }
        items.push_back(std::move(item));
    }
    return {200, apiOkJson("catalog", {{"id", browser.id}, {"version", std::to_string(browser.version)},
                                      {"total", total}, {"offset", offset}, {"items", std::move(items)}})};
}

ApiResponse openCatalog(const std::string &body)
{
    const auto payload = nlohmann::json::parse(body, nullptr, false);
    if (!payload.is_object() || !payload.contains("dbPath") || !payload["dbPath"].is_string() ||
        !payload.contains("sort") || !payload["sort"].is_string() ||
        (payload.contains("anchor") && (!payload["anchor"].is_number_integer() ||
                                        payload["anchor"] <= 0 || payload["anchor"] > INT_MAX)))
    {
        return badRequest("catalog requires dbPath, sort and an optional positive anchor ID");
    }
    const auto sort = payload["sort"].get<std::string>();
    if (sort != "color" && sort != "usage" && sort != "similarity")
    {
        return badRequest("unknown catalog sort");
    }
    static std::mutex buildMutex;
    const std::unique_lock<std::mutex> lock(buildMutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        return resultResponse(400, ServiceResult::failure(1, "another catalog is being prepared", "library_busy"));
    }
    auto browser = std::make_shared<LibraryBrowser>();
    const auto result = browseLibrary(payload["dbPath"].get<std::string>(), sort, payload.value("anchor", 0), *browser);
    if (!result.ok)
    {
        return resultResponse(400, result);
    }
    browserStore().add(browser);
    return catalogPage(*browser, 0);
}

ApiResponse readCatalog(const ApiRequest &request)
{
    const auto browser = browserStore().get(request.id);
    if (!browser)
    {
        return resultResponse(404, ServiceResult::failure(1, "catalog has expired", "catalog_expired"));
    }
    if (request.operation == ApiOperation::GetCatalog)
    {
        size_t offset = 0;
        if (!previewNumber(request.query, "offset", offset, false))
        {
            return badRequest("invalid catalog offset");
        }
        return catalogPage(*browser, offset);
    }
    int id = 0;
    const auto &key = request.artifactId;
    const auto dash = key.find('-');
    if (dash == std::string::npos)
    {
        return badRequest("catalog image requires an ID and version");
    }
    const auto result = std::from_chars(key.data(), key.data() + dash, id);
    if (result.ec != std::errc{} || result.ptr != key.data() + dash)
    {
        return badRequest("invalid catalog image ID");
    }
    const auto found = browser->positions.find(id);
    if (found == browser->positions.end())
    {
        return previewImageNotFound();
    }
    const auto asset = previewAsset(browser->ordered[found->second]);
    auto version = asset.version();
    std::replace(version.begin(), version.end(), ':', '-');
    if (key.substr(dash + 1) != version)
    {
        return previewConflict("catalog image changed; refresh the catalog");
    }
    return thumbnailResponse(asset, 128);
}

std::optional<ApiResponse> applyPreviewEdits(const nlohmann::json &edits,
                                             std::shared_ptr<const MosaicSelection> &selection)
{
    if (!edits.is_array() || !selection || !selection->catalog || edits.size() > selection->indices.size())
    {
        return badRequest("previewEdits requires a preview and an array of tile replacements");
    }
    if (edits.empty())
    {
        return {};
    }
    auto updated = std::make_shared<MosaicSelection>(*selection);
    std::unordered_set<size_t> seen;
    for (const auto &edit : edits)
    {
        if (!edit.is_object() || edit.size() != 3 || !edit.contains("position") || !edit.contains("imageId") ||
            !hasString(edit, "version") || !edit["position"].is_number_unsigned() ||
            !edit["imageId"].is_number_unsigned())
        {
            return badRequest("preview edit requires position, imageId and version");
        }
        const auto position = edit["position"].get<uint64_t>();
        const auto imageId = edit["imageId"].get<uint64_t>();
        if (position >= updated->indices.size() || imageId == 0 || imageId > INT_MAX || !seen.insert(position).second)
        {
            return badRequest("invalid or repeated preview edit");
        }
        const auto *entry = selection->catalog->find(static_cast<int>(imageId));
        if (!entry)
        {
            return badRequest("replacement image is not in the preview library");
        }
        const auto asset = previewAsset(*entry);
        if (!asset.available || asset.version() != edit["version"].get_ref<const std::string &>())
        {
            return previewConflict("replacement image changed; select it again after refreshing");
        }
        if (updated->indices[position] != entry->index)
        {
            updated->indices[position] = entry->index;
            updated->imageIds[position] = entry->id;
            updated->assets.insert_or_assign(entry->id, asset);
            ++updated->manualEdits;
        }
    }
    selection = std::move(updated);
    return {};
}

ApiResponse pinComparison(const std::string &body, JobManager &jobs)
{
    const auto payload = nlohmann::json::parse(body, nullptr, false);
    if (!payload.is_object() || !hasString(payload, "slot") || !hasString(payload, "previewId") ||
        (payload["slot"] != "A" && payload["slot"] != "B"))
    {
        return badRequest("pin requires slot A or B and a previewId");
    }
    return jobs.pinPreview(payload["slot"] == "A" ? 0 : 1, payload["previewId"].get<std::string>())
        ? ApiResponse{200, apiOkJson()} : previewConflict("preview expired");
}

ApiResponse submitComparison(const std::string &body, JobManager &jobs)
{
    const auto payload = nlohmann::json::parse(body, nullptr, false);
    if (!payload.is_object() || !payload.contains("a") || !payload.contains("b") ||
        !payload["a"].is_object() || !payload["b"].is_object() ||
        !hasString(payload["a"], "previewId") || !hasString(payload["b"], "previewId"))
    {
        return badRequest("comparison requires two previews");
    }
    PreviewSnapshot a, b;
    if (!jobs.getPreview(payload["a"]["previewId"].get<std::string>(), a) || !a.selection ||
        !jobs.getPreview(payload["b"]["previewId"].get<std::string>(), b) || !b.selection)
    {
        return previewConflict("comparison preview expired");
    }
    if (a.selection->editKey != b.selection->editKey || a.selection->dbPath != b.selection->dbPath ||
        a.selection->targetVersion != b.selection->targetVersion || a.selection->libraryVersion != b.selection->libraryVersion ||
        a.selection->columns != b.selection->columns || a.selection->rows != b.selection->rows || a.seed != b.seed)
    {
        return previewConflict("comparison needs the same target, library, grid and random seed");
    }
    if (payload["a"].contains("previewEdits"))
    {
        if (auto error = applyPreviewEdits(payload["a"]["previewEdits"], a.selection))
        {
            return std::move(*error);
        }
    }
    if (payload["b"].contains("previewEdits"))
    {
        if (auto error = applyPreviewEdits(payload["b"]["previewEdits"], b.selection))
        {
            return std::move(*error);
        }
    }
    const auto id = jobs.submitPreview(std::move(a.request), a.seed, std::move(a.selection), std::move(b.selection));
    return {202, apiOkJson("preview", {{"id", id}, {"state", "queued"}})};
}

} // namespace

ApiRequest apiOperationRequest(ApiOperation operation, ApiRequestContext context)
{
    ApiRequest request;
    request.operation = operation;
    request.query = std::move(context.query);
    request.body = std::move(context.body);
    request.id = std::move(context.id);
    request.artifactId = std::move(context.artifactId);
    request.entryName = context.entryName;
    return request;
}

ApiRequest apiEndpointRequest(const ApiEndpointMetadata& endpoint, ApiRequestContext context)
{
    return apiOperationRequest(endpoint.operation, std::move(context));
}

ApiRequest apiRequest(ApiOperation operation)
{
    return apiOperationRequest(operation);
}

ApiRequest apiBodyRequest(ApiOperation operation, std::string body)
{
    ApiRequestContext context;
    context.body = std::move(body);
    return apiOperationRequest(operation, std::move(context));
}

ApiRequest apiQueryRequest(ApiOperation operation, ApiQueryParams query, std::string body)
{
    ApiRequestContext context;
    context.query = std::move(query);
    context.body = std::move(body);
    return apiOperationRequest(operation, std::move(context));
}

ApiRequest apiJobRequest(ApiOperation operation, std::string id)
{
    ApiRequestContext context;
    context.id = std::move(id);
    return apiOperationRequest(operation, std::move(context));
}

ApiRequest apiInfoRequest(const char* entryName)
{
    ApiRequestContext context;
    context.entryName = entryName;
    return apiOperationRequest(ApiOperation::Info, std::move(context));
}

std::vector<std::string> apiQueryKeyList(ApiOperation operation)
{
    static const auto endpoints = apiEndpointMetadata();
    for (const auto& endpoint : endpoints) {
        if (endpoint.operation == operation) {
            return endpoint.queryKeys;
        }
    }
    return {};
}

std::vector<const char*> apiQueryKeys(ApiOperation operation)
{
    static const auto endpoints = apiEndpointMetadata();
    std::vector<const char*> keys;
    for (const auto& endpoint : endpoints) {
        if (endpoint.operation != operation) continue;
        keys.reserve(endpoint.queryKeys.size());
        for (const auto& key : endpoint.queryKeys) keys.push_back(key.c_str());
        break;
    }
    return keys;
}

std::vector<std::string> apiAcceptedQueryKeyList(ApiOperation operation)
{
    static const auto endpoints = apiEndpointMetadata();
    for (const auto& endpoint : endpoints) {
        if (endpoint.operation == operation) return endpoint.acceptedQueryKeys;
    }
    return {};
}

ApiResponse handleApiRequest(const ApiRequest& request, JobManager& jobs)
{
    switch (request.operation)
    {
    case ApiOperation::Endpoints:
        return apiEndpoints();
    case ApiOperation::Info: {
        auto response = apiInfo(request.entryName);
        std::error_code error;
        const auto directory = std::filesystem::current_path(error);
        response.body["info"]["runtime"] = {{"sessionId", jobs.sessionId()},
                                            {"workingDirectory", error ? "" : directory.u8string()},
                                            {"finishedJobLimit", 50}};
        return response;
    }
    case ApiOperation::Ping:
        return apiPing();
    case ApiOperation::Mosaic:
        return apiMosaic(request.body, jobs);
    case ApiOperation::PlanMosaic:
        return mosaicPlanResponse(request.body);
    case ApiOperation::MosaicCoverage:
        return mosaicCoverageResponse(request.body);
    case ApiOperation::OpenCatalog:
        return openCatalog(request.body);
    case ApiOperation::GetCatalog:
    case ApiOperation::GetCatalogImage:
        return readCatalog(request);
    case ApiOperation::SubmitMosaicSample:
        return apiSubmitMosaicJob(request.body, jobs, true);
    case ApiOperation::SubmitPreview:
        return submitPreview(request.body, jobs);
    case ApiOperation::GetPreview:
        return getPreview(request.id, jobs);
    case ApiOperation::PreviewQuality:
        return previewQuality(request.body, jobs);
    case ApiOperation::PinComparison:
        return pinComparison(request.body, jobs);
    case ApiOperation::SubmitComparison:
        return submitComparison(request.body, jobs);
    case ApiOperation::ClosePreview:
        return jobs.closePreview(request.id) ? ApiResponse{200, apiOkJson()} : previewNotFound();
    case ApiOperation::GetPreviewImage:
        return previewImage(request, jobs);
    case ApiOperation::GetPreviewRecommendations:
        return previewRecommendations(request, jobs);
    case ApiOperation::BrowsePreviewLibrary:
        return browsePreviewLibrary(request, jobs);
    case ApiOperation::SubmitMosaicJob:
        return apiSubmitMosaicJob(request.body, jobs);
    case ApiOperation::ProjectInfo:
    case ApiOperation::SubmitRenderJob:
    case ApiOperation::SubmitProjectCheckJob:
    case ApiOperation::SubmitProjectPackJob:
    {
        ProjectRequest project;
        RequestError error;
        if (!parseProjectRequestJson(request.body, request.operation, project, error))
        {
            return badRequest(error);
        }
        try
        {
            if (request.operation == ApiOperation::ProjectInfo)
            {
                return {200, apiOkJson("project", ProjectService().info(project.projectPath))};
            }
            const auto type = request.operation == ApiOperation::SubmitRenderJob         ? "render"
                              : request.operation == ApiOperation::SubmitProjectCheckJob ? "project-check"
                                                                                         : "project-pack";
            const auto id = jobs.submitProject(type, std::move(project));
            JobSnapshot snapshot;
            jobs.getJob(id, snapshot);
            return {202, apiJobJson(snapshot)};
        }
        catch (const std::exception &exception)
        {
            return badRequest(exception.what());
        }
    }
    case ApiOperation::SubmitBuildJob:
        return apiSubmitBuildJob(request.body, jobs);
    case ApiOperation::ListJobs:
        return apiListJobs(jobs);
    case ApiOperation::ClearFinishedJobs:
        return apiClearFinishedJobs(jobs);
    case ApiOperation::ListHistory:
    {
        nlohmann::json records;
        if (!jobs.history() || !jobs.history()->list(records))
        {
            return internalError("local history is unavailable");
        }
        return {200, apiOkJson("history", std::move(records))};
    }
    case ApiOperation::ClearHistory:
    {
        int removed = 0;
        if (!jobs.history() || !jobs.history()->clear(removed))
        {
            return internalError("local history is unavailable");
        }
        return {200, apiOkJson("removed", removed)};
    }
    case ApiOperation::GetJobEvents:
        return apiGetJobEvents(request, jobs);
    case ApiOperation::GetJobArtifact:
        return apiGetJobArtifact(request, jobs);
    case ApiOperation::GetJob:
        return apiGetJob(request.id, jobs);
    case ApiOperation::CancelJob:
        return apiCancelJob(request.id, jobs);
    case ApiOperation::DatabaseStats:
        return apiDatabaseStats(request.query, request.body);
    case ApiOperation::DatabaseHealth:
        return apiDatabaseHealth(request.query, request.body);
    case ApiOperation::DatabaseUsage:
        return apiDatabaseUsage(request.query, request.body);
    case ApiOperation::DatabaseUsageExport:
        return apiDatabaseUsageExport(request.query, request.body);
    case ApiOperation::DatabasePurge:
        return apiDatabasePurge(request.query, request.body);
    case ApiOperation::Inspect:
        return apiInspect(request.query, request.body);
    }
    return internalError("unknown API operation");
}

std::vector<std::string> validateApiResponseContract(const ApiEndpointMetadata& endpoint,
                                                      const ApiResponse& response)
{
    std::vector<std::string> errors;
    const std::string operationName = apiOperationName(endpoint.operation);

    if (!response.body.is_object()) {
        errors.push_back("API response body is not an object: " + operationName);
        return errors;
    }
    if (!hasBool(response.body, "ok")) {
        errors.push_back("API response body missing boolean ok: " + operationName);
        return errors;
    }

    const bool ok = response.body["ok"].get<bool>();
    if (response.status >= 200 && response.status <= 299) {
        if (response.status != endpoint.successStatus) {
            errors.push_back("API response success status does not match metadata: " +
                             operationName + " " + statusText(response.status));
        }
        if (!ok) {
            errors.push_back("API success response has ok=false: " + operationName);
        }
        if (!endpoint.responseKey.empty() && !response.body.contains(endpoint.responseKey)) {
            errors.push_back("API success response missing responseKey: " +
                             operationName + " " + endpoint.responseKey);
        }
        return errors;
    }

    if (response.status < 400 || response.status > 599) {
        errors.push_back("API response status is neither success nor error: " +
                         operationName + " " + statusText(response.status));
        return errors;
    }
    if (ok) {
        errors.push_back("API error response has ok=true: " + operationName);
    }

    bool statusDeclared = false;
    bool shapeMatched = false;
    for (const auto& item : endpoint.errorResponses) {
        if (item.status != response.status) continue;
        statusDeclared = true;
        if (matchesErrorShape(response.body, item.shape, item.responseKey)) {
            shapeMatched = true;
            break;
        }
    }
    if (!statusDeclared) {
        errors.push_back("API error status is not declared in metadata: " +
                         operationName + " " + statusText(response.status));
    } else if (!shapeMatched) {
        errors.push_back("API error response shape does not match metadata: " +
                         operationName + " " + statusText(response.status));
    }

    return errors;
}

ApiResponse apiEndpoints()
{
    return {200, apiEndpointsResponseJson()};
}

ApiResponse apiInfo(const char* entryName)
{
    return {200, apiInfoResponseJson(entryName)};
}

ApiResponse apiPing()
{
    return {200, apiOkJson("message", "pong")};
}

ApiResponse apiMosaic(const std::string& body, JobManager& jobs)
{
    MosaicRequest request;
    RequestError error;
    if (!parseMosaicRequestJson(body, request, error))
    {
        return badRequest(error);
    }

    try
    {
        const auto result = MosaicService().run(request);
        return resultResponse(result.ok ? 200 : (result.exitCode == 3 ? 500 : 400), result);
    }
    catch (const std::exception& e)
    {
        return internalError(e.what());
    }
    catch (...)
    {
        return internalError("internal error");
    }
}

ApiResponse apiSubmitMosaicJob(const std::string& body, JobManager& jobs, bool sampleSize)
{
    MosaicRequest request;
    RequestError error;
    auto payload = nlohmann::json::parse(body, nullptr, false);
    std::string previewId;
    nlohmann::json edits;
    const bool hasEdits = payload.is_object() && payload.contains("previewEdits");
    if (hasEdits)
    {
        edits = std::move(payload["previewEdits"]);
        payload.erase("previewEdits");
    }
    if (payload.is_object() && payload.contains("previewId"))
    {
        if (!payload["previewId"].is_string() || payload["previewId"].get_ref<const std::string &>().empty())
        {
            return badRequest("previewId must be a non-empty string");
        }
        previewId = payload["previewId"].get<std::string>();
        payload.erase("previewId");
    }
    if ((hasEdits || sampleSize) && previewId.empty())
    {
        return badRequest(sampleSize ? "size sampling requires previewId" : "previewEdits requires previewId");
    }
    if (!parseMosaicRequestJson(previewId.empty() ? body : payload.dump(), request, error))
    {
        return badRequest(error);
    }

    try
    {
        PreviewSnapshot preview;
        if (!previewId.empty() &&
            (!jobs.getPreview(previewId, preview) || !preview.selection ||
             !MosaicEngine::selectionCurrent(request.inputPath, resolveDbPathForService(request.dbPath),
                                             request.config, *preview.selection)))
        {
            return previewConflict("preview expired or target, library, or matching parameters changed");
        }
        if (hasEdits)
        {
            if (auto error = applyPreviewEdits(edits, preview.selection))
            {
                return std::move(*error);
            }
        }
        if (sampleSize)
        {
            const auto id = jobs.submitPreview(std::move(request), 0, std::move(preview.selection));
            return {202, apiOkJson("preview", {{"id", id}, {"state", "queued"}})};
        }
        const std::string jobId = jobs.submitMosaic(std::move(request), std::move(preview.selection));
        JobSnapshot snapshot;
        jobs.getJob(jobId, snapshot);
        return {202, apiJobJson(snapshot)};
    } catch (const std::exception& e) {
        return internalError(e.what());
    } catch (...) {
        return internalError("internal error");
    }
}

ApiResponse apiSubmitBuildJob(const std::string& body, JobManager& jobs)
{
    BuildRequest request;
    RequestError error;
    if (!parseBuildRequestJson(body, request, error)) return badRequest(error);

    try {
        const std::string jobId = jobs.submitBuild(request);
        JobSnapshot snapshot;
        jobs.getJob(jobId, snapshot);
        return {202, apiJobJson(snapshot)};
    } catch (const std::exception& e) {
        return internalError(e.what());
    } catch (...) {
        return internalError("internal error");
    }
}

ApiResponse apiListJobs(const JobManager& jobs)
{
    auto body = apiJobsJson(jobs.listJobs());
    body["sessionId"] = jobs.sessionId();
    return {200, body};
}

ApiResponse apiClearFinishedJobs(JobManager& jobs)
{
    return {200, apiRemovedJobsJson(jobs.clearFinishedJobs())};
}

ApiResponse apiGetJob(const std::string& id, const JobManager& jobs)
{
    JobSnapshot snapshot;
    if (!jobs.getJob(id, snapshot)) {
        return {404, apiErrorJson("job not found")};
    }
    auto body = apiJobJson(snapshot);
    body["sessionId"] = jobs.sessionId();
    return {200, body};
}

ApiResponse apiGetJobEvents(const ApiRequest &request, const JobManager &jobs)
{
    uint64_t after = 0, limit = 100;
    for (auto item : {std::pair<const char *, uint64_t *>("after", &after), {"limit", &limit}})
    {
        const auto found = request.query.find(item.first);
        if (found == request.query.end())
        {
            continue;
        }
        const auto &text = found->second;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *item.second);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            (item.first == std::string("limit") && limit == 0))
        {
            RequestError error;
            error.field = item.first;
            error.message = error.field + " must be a valid nonnegative integer (limit >=1)";
            return badRequest(error);
        }
    }
    EventPage page;
    if (!jobs.getEvents(request.id, after, static_cast<size_t>(std::min<uint64_t>(limit, 100)), page))
    {
        return {404, apiErrorJson("job not found")};
    }
    auto body = eventPageToJson(page);
    body["sessionId"] = jobs.sessionId();
    return {200, body};
}

ApiResponse apiGetJobArtifact(const ApiRequest &request, const JobManager &jobs)
{
    JobSnapshot job;
    if (!jobs.getJob(request.id, job))
    {
        return {404, apiErrorJson("job not found")};
    }
    for (const auto &item : job.result.runtime.artifacts)
    {
        if (item.id != request.artifactId || item.directory)
        {
            continue;
        }
        if (item.kind == "project-entry" || item.kind == "project-manifest")
        {
            return {409,
                    apiErrorJson("pack the complete project before downloading; the HTML entry alone is incomplete")};
        }
        try
        {
            const auto path = std::filesystem::u8path(item.path);
            if (!std::filesystem::exists(path))
            {
                return {404, apiErrorJson("artifact no longer exists")};
            }
            if (std::filesystem::canonical(path).u8string() != item.path || !std::filesystem::is_regular_file(path) ||
                std::filesystem::file_size(path) != item.bytes ||
                std::filesystem::last_write_time(path) != item.modified)
            {
                auto response = apiErrorJson("artifact was changed after this job completed");
                response["diagnostic"] = {{"code", "artifact_changed"}, {"message", response["message"]}};
                return {409, response};
            }
            return {200, apiOkJson("artifact", artifactToJson(item)), item};
        }
        catch (const std::filesystem::filesystem_error &error)
        {
            return {500, apiErrorJson(error.what())};
        }
    }
    return {404, apiErrorJson("artifact is not registered or is a directory")};
}

ApiResponse apiCancelJob(const std::string& id, JobManager& jobs)
{
    JobSnapshot snapshot;
    if (jobs.cancelQueuedJob(id, snapshot)) {
        return {200, apiJobJson(snapshot)};
    }
    if (snapshot.id.empty()) {
        return {404, apiErrorJson("job not found")};
    }
    return {409, apiJobErrorJson("only queued jobs can be canceled", snapshot)};
}

ApiResponse apiDatabaseStats(const ApiQueryParams& query, const std::string& body)
{
    DatabaseRequest request;
    RequestError error;
    if (!parseDatabaseRequestApi(query, body, request, error)) return badRequest(error);

    DatabaseService service;
    const auto result = service.stats(request);
    if (!result.status.ok) return resultResponse(500, result.status);
    return {200, apiPayloadJson(result.status, "stats", databaseStatsToJson(result.stats))};
}

ApiResponse apiDatabaseHealth(const ApiQueryParams& query, const std::string& body)
{
    DatabaseRequest request;
    RequestError error;
    if (!parseDatabaseRequestApi(query, body, request, error)) return badRequest(error);

    DatabaseService service;
    const auto result = service.health(request);
    if (!result.status.ok) return resultResponse(500, result.status);
    return {200, apiPayloadJson(result.status, "health", databaseHealthToJson(result.health))};
}

ApiResponse apiDatabaseUsage(const ApiQueryParams& query, const std::string& body)
{
    DatabaseUsageRequest request;
    RequestError error;
    if (!parseDatabaseUsageRequestApi(query, body, request, error)) return badRequest(error);

    DatabaseService service;
    const auto result = service.usage(request);
    if (!result.status.ok) return resultResponse(500, result.status);
    return {200, apiPayloadJson(result.status, "usage", databaseUsageToJson(result.usage))};
}

ApiResponse apiDatabaseUsageExport(const ApiQueryParams& query, const std::string& body)
{
    DatabaseUsageExportRequest request;
    RequestError error;
    if (!parseDatabaseUsageExportRequestApi(query, body, request, error)) return badRequest(error);

    DatabaseService service;
    const auto result = service.exportUsage(request);
    return {
        result.status.ok ? 200 : databaseMaintenanceErrorStatus(result.status),
        apiPayloadJson(result.status, "export", databaseUsageExportToJson(result.exportInfo))
    };
}

ApiResponse apiDatabasePurge(const ApiQueryParams& query, const std::string& body)
{
    DatabasePurgeRequest request;
    RequestError error;
    if (!parseDatabasePurgeRequestApi(query, body, request, error)) return badRequest(error);

    DatabaseService service;
    const auto result = service.purge(request);
    return {
        result.status.ok ? 200 : databaseMaintenanceErrorStatus(result.status),
        apiPayloadJson(result.status, "purge", databasePurgeToJson(result.purge))
    };
}

ApiResponse apiInspect(const ApiQueryParams& query, const std::string& body)
{
    InspectRequest request;
    RequestError error;
    if (!parseInspectRequestApi(query, body, request, error)) return badRequest(error);

    InspectService service;
    const auto result = service.inspect(request);
    if (!result.status.ok) return resultResponse(400, result.status);
    return {200, apiPayloadJson(result.status, "inspect", inspectResultToJson(result))};
}

} // namespace mosaicraft
