#include "EditorSession.h"

#include "ApiJson.h"
#include "ApiRequestParser.h"
#include "Base64.h"
#include "BrightnessAdjustment.h"
#include "ImageCache.h"
#include "LibraryMaintenance.h"
#include "MosaicPlanning.h"
#include "MosaicSelection.h"
#include "PreviewCandidates.h"
#include "TileCompositor.h"
#include "UnicodeIO.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <climits>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace mosaicraft
{
using nlohmann::json;

namespace
{
constexpr int previewEdge = 1800;

MosaicRequest parseEditorRequest(const json &payload)
{
    const auto fields = payload.find("request");
    if (fields == payload.end() || !fields->is_object())
    {
        throw std::invalid_argument("editor requires a request object; use the matching desktop version");
    }
    MosaicRequest request;
    RequestError error;
    if (!parseMosaicRequestJson(fields->dump(), request, error))
    {
        throw std::invalid_argument(error.message);
    }
    return request;
}

using FileStamp = std::pair<uintmax_t, std::filesystem::file_time_type>;

FileStamp fileStamp(const std::string &path)
{
    std::error_code error;
    const auto file = u8path(path);
    const auto size = std::filesystem::file_size(file, error);
    if (error) return {};
    const auto modified = std::filesystem::last_write_time(file, error);
    return error ? FileStamp{} : FileStamp{size, modified};
}

std::array<FileStamp, 3> inputStamps(const MosaicRequest &request, const MosaicSelection &selection)
{
    return {fileStamp(request.inputPath), fileStamp(selection.dbPath), fileStamp(selection.dbPath + "-wal")};
}

std::string encodeImage(const cv::Mat &image)
{
    std::vector<unsigned char> jpeg;
    if (!cv::imencode(".jpg", image, jpeg, {cv::IMWRITE_JPEG_QUALITY, 91}))
    {
        throw std::runtime_error("cannot encode editor preview");
    }
    return encodeBase64(jpeg);
}

size_t readPosition(const json &payload, const MosaicSelection &selection)
{
    const auto &value = payload.at("position");
    if (!value.is_number_unsigned() || value >= selection.indices.size())
    {
        throw std::invalid_argument("invalid tile position");
    }
    return value.get<size_t>();
}

size_t replaceAsset(MosaicSelection &selection, const json &payload)
{
    const auto &position = payload.at("position");
    const auto &id = payload.at("imageId");
    const auto &version = payload.at("version");
    if (!position.is_number_unsigned() || position.get<uint64_t>() > SIZE_MAX)
    {
        throw std::invalid_argument("invalid tile position");
    }
    if (!id.is_number_unsigned() || id.get<uint64_t>() > INT_MAX)
    {
        throw std::invalid_argument("invalid replacement image ID");
    }
    if (!version.is_string())
    {
        throw std::invalid_argument("replacement image changed or is unavailable");
    }
    const auto tile = position.get<size_t>();
    switch (selection.replaceAsset(tile, id.get<int>(), version.get_ref<const std::string &>()))
    {
    case PreviewEditResult::Applied:
    case PreviewEditResult::Unchanged:
        return tile;
    case PreviewEditResult::InvalidPosition:
        throw std::invalid_argument("invalid tile position");
    case PreviewEditResult::InvalidImageId:
        throw std::invalid_argument("invalid replacement image ID");
    default:
        throw std::invalid_argument("replacement image changed or is unavailable");
    }
}

int boundary(int cell, int cells, int pixels)
{
    return static_cast<int>((int64_t(cell) * pixels + cells - 1) / cells);
}

struct EditorFrame
{
    cv::Mat image, reference, nativeReference;
    std::filesystem::path directory;
    cv::Size tileSize;
    MosaicEngine::Config renderConfig;
    std::unordered_map<int, uint32_t> sharedTiles;
    uint32_t nextTile = 0;

    void paintOverview(const MosaicSelection &selection, size_t position, ImageCache &cache)
    {
        const auto column = static_cast<int>(position % selection.columns);
        const auto row = static_cast<int>(position / selection.columns);
        const auto left = boundary(column, selection.columns, image.cols);
        const auto right = boundary(column + 1, selection.columns, image.cols);
        const auto top = boundary(row, selection.rows, image.rows);
        const auto bottom = boundary(row + 1, selection.rows, image.rows);
        if (left == right || top == bottom)
        {
            return;
        }
        const cv::Rect region(left, top, right - left, bottom - top);
        auto tile = image(region);
        const auto id = selection.imageIds[position];
        const auto asset = selection.assets.find(id);
        if (asset != selection.assets.end())
        {
            if (auto pixels = cache.getShared(id, asset->second.path, region.width, region.height))
            {
                pixels->copyTo(tile);
                if (renderConfig.colorAdjust)
                {
                    detail::adjustBrightnessAt(tile, renderConfig.colorStrength, renderConfig.seed.value_or(0), position);
                }
                if (!reference.empty())
                {
                    cv::addWeighted(tile, 1 - renderConfig.overlayStrength, reference(region), renderConfig.overlayStrength, 0, tile);
                }
                return;
            }
        }
        tile.setTo(cv::Scalar());
    }

    uint32_t writeTile(const MosaicSelection &selection, size_t position, ImageCache &cache)
    {
        const int id = selection.imageIds[position];
        const auto asset = selection.assets.find(id);
        if (asset == selection.assets.end()) return 0;
        const bool shared = !renderConfig.colorAdjust && nativeReference.empty();
        if (shared)
        {
            const auto previous = sharedTiles.find(id);
            if (previous != sharedTiles.end()) return previous->second;
        }
        const auto native = cache.getShared(id, asset->second.path, tileSize.width, tileSize.height);
        if (!native) return 0;
        auto pixels = shared ? *native : native->clone();
        if (renderConfig.colorAdjust)
            detail::adjustBrightnessAt(pixels, renderConfig.colorStrength, renderConfig.seed.value_or(0), position);
        if (!nativeReference.empty())
        {
            cv::Mat targetTile;
            TileCompositor(nativeReference, selection.columns, selection.rows, tileSize).sample(position, targetTile);
            cv::addWeighted(pixels, 1 - renderConfig.overlayStrength, targetTile, renderConfig.overlayStrength, 0, pixels);
        }
        const auto key = ++nextTile;
        const auto file = pathToUtf8(directory / (std::to_string(key) + ".png"));
        if (!key || !imwriteUnicode(file, pixels, {cv::IMWRITE_PNG_COMPRESSION, 1}))
            throw std::runtime_error("cannot write original-size preview tile");
        if (shared) sharedTiles.emplace(id, key);
        return key;
    }

    void compose(const MosaicSelection &selection, const MosaicRequest &request, const PreviewWorkspace &workspace,
                 std::filesystem::path files, ImageCache &cache)
    {
        if (!std::filesystem::is_directory(files) || !std::filesystem::is_empty(files))
            throw std::invalid_argument("editor preview requires an empty temporary directory");
        directory = std::move(files);
        renderConfig = request.config;
        orientOutputTile(renderConfig, selection.featureWidth, selection.featureHeight);
        tileSize = {renderConfig.nativeTileW, renderConfig.nativeTileH};
        const auto width = int64_t(tileSize.width) * selection.columns;
        const auto height = int64_t(tileSize.height) * selection.rows;
        if (width <= 0 || height <= 0 || width > INT_MAX || height > INT_MAX)
            throw std::invalid_argument("editor preview dimensions exceed INT_MAX");
        if (renderConfig.overlayStrength > 0)
        {
            nativeReference = workspace.target;
            if (nativeReference.empty())
            {
                nativeReference = imreadUnicode(request.inputPath, cv::IMREAD_COLOR);
                TargetGeometry geometry;
                std::string error;
                if (nativeReference.empty() || !resolveTargetGeometry(nativeReference.cols, nativeReference.rows,
                                                                      renderConfig, geometry, error))
                    throw std::runtime_error("cannot prepare the editor overlay reference");
                resizeTarget(nativeReference, geometry);
            }
            padTarget(nativeReference, renderConfig, selection.columns, selection.rows);
        }

        // The GUI reads native tiles when zoomed in. Only the fit overview is reduced.
        std::ofstream index(directory / "cells.bin", std::ios::binary);
        index.exceptions(std::ios::failbit | std::ios::badbit);
        for (size_t position = 0; position < selection.imageIds.size(); ++position)
        {
            const auto key = writeTile(selection, position, cache);
            index.write(reinterpret_cast<const char *>(&key), sizeof(key));
        }
        index.close();
        const double scale = std::min(1.0, double(previewEdge) / std::max(width, height));
        const cv::Size size(std::max(1, cvRound(width * scale)), std::max(1, cvRound(height * scale)));
        image.create(size, CV_8UC3);
        if (!nativeReference.empty()) cv::resize(nativeReference, reference, size, 0, 0, cv::INTER_AREA);
        for (int top = 0; top < size.height;)
        {
            const int row = static_cast<int>(int64_t(top) * selection.rows / size.height);
            for (int left = 0; left < size.width;)
            {
                const int column = static_cast<int>(int64_t(left) * selection.columns / size.width);
                paintOverview(selection, size_t(row) * selection.columns + column, cache);
                left = boundary(column + 1, selection.columns, size.width);
            }
            top = boundary(row + 1, selection.rows, size.height);
        }
    }
};

class EditorSession
{
public:
    json dispatch(const json &payload)
    {
        const auto operation = payload.at("op").get<std::string>();
        if (operation == "maintain-library")
        {
            return maintainDesktopLibrary(payload);
        }
        if (operation == "preview")
        {
            auto nextRequest = parseEditorRequest(payload);
            const auto seed = nextRequest.config.seed ? *nextRequest.config.seed : std::random_device{}();
            nextRequest.config.seed = seed;
            MosaicSelection nextSelection;
            auto nextWorkspace = std::exchange(workspace, PreviewWorkspace{});
            const auto status = MosaicService().preview(nextRequest, seed, nextWorkspace, canceled, nextSelection);
            if (!status.ok)
            {
                return serviceResultToJson(status);
            }
            const auto &config = nextRequest.config;
            auto layoutIdentity = std::to_string(nextSelection.targetVersion) + ":" +
                std::to_string(config.outW) + ":" + std::to_string(config.outH) + ":" +
                std::to_string(config.tileW) + ":" + std::to_string(config.tileH) + ":" + nextSelection.dbPath;
            // 百分比改变网格，未使用百分比的项目保持已有换图标识。
            if (config.targetPercentMilli > 0)
            {
                layoutIdentity += ":" + std::to_string(config.targetPercentMilli) + "%";
            }
            const auto layout = std::to_string(previewFileVersion({layoutIdentity.begin(), layoutIdentity.end()}));
            const auto edits = payload.value("edits", json::array());
            if (!edits.is_array() || edits.size() > nextSelection.indices.size() ||
                (!edits.empty() && payload.value("layout", std::string{}) != layout))
            {
                return {{"ok", false}, {"code", "saved_edits_stale"},
                        {"message", "saved edits no longer match the target, library, or grid"}};
            }
            try
            {
                for (const auto &edit : edits)
                {
                    replaceAsset(nextSelection, edit);
                }
            }
            catch (const std::invalid_argument &error)
            {
                return {{"ok", false}, {"code", "saved_edits_stale"}, {"message", error.what()}};
            }
            EditorFrame nextFrame;
            auto nextCache = std::make_unique<ImageCache>(64ULL * 1024 * 1024);
            nextFrame.compose(nextSelection, nextRequest, nextWorkspace,
                              u8path(payload.at("previewDirectory").get<std::string>()), *nextCache);
            auto response = json{{"ok", true}, {"image", encodeImage(nextFrame.image)},
                                 {"reference", encodeBase64(nextSelection.referenceJpeg)}, {"seed", seed},
                                 {"columns", nextSelection.columns}, {"rows", nextSelection.rows},
                                 {"tileWidth", nextFrame.tileSize.width}, {"tileHeight", nextFrame.tileSize.height},
                                 {"layout", layout}};
            selection = std::move(nextSelection);
            request = std::move(nextRequest);
            workspace = std::move(nextWorkspace);
            sources = inputStamps(request, selection);
            frame = std::move(nextFrame);
            cache = std::move(nextCache);
            return response;
        }
        if (!cache)
        {
            throw std::invalid_argument("generate a preview first");
        }
        if (operation == "export")
        {
            auto output = parseEditorRequest(payload);
            output.config.seed = request.config.seed;
            RuntimeContext runtime(false);
            TaskResourceMonitor monitor(runtime, output.resourceMonitor);
            return serviceResultToJson(MosaicService().run(output, runtime, &selection));
        }
        if (operation == "replace")
        {
            const auto position = readPosition(payload, selection);
            const auto previousId = selection.imageIds[position];
            const auto previousIndex = selection.indices[position];
            const auto previousEdits = selection.manualEdits;
            replaceAsset(selection, payload);
            auto oldOverview = frame.image;
            try
            {
                const auto key = frame.writeTile(selection, position, *cache);
                if (!key) throw std::runtime_error("cannot read replacement preview tile");
                frame.image = oldOverview.clone();
                frame.paintOverview(selection, position, *cache);
                return {{"ok", true}, {"image", encodeImage(frame.image)}, {"tileKey", key},
                        {"imageId", selection.imageIds[position]}};
            }
            catch (...)
            {
                selection.imageIds[position] = previousId;
                selection.indices[position] = previousIndex;
                selection.manualEdits = previousEdits;
                frame.image = std::move(oldOverview);
                throw;
            }
        }
        if (operation == "candidates")
        {
            const auto position = readPosition(payload, selection);
            const auto currentSources = inputStamps(request, selection);
            if (currentSources != sources)
            {
                if (!MosaicEngine::selectionCurrent(request.inputPath, selection.dbPath, request.config, selection))
                {
                    return serviceResultToJson(ServiceResult::failure(1,
                        "the target or library changed; generate a new preview", "preview_stale"));
                }
                sources = currentSources;
            }
            PreviewCandidateRow row;
            const auto status = previewCandidates(request, selection, workspace, position, request.config.candidates, row);
            if (!status.ok)
            {
                return serviceResultToJson(status);
            }
            json items = json::array();
            const auto append = [&](int imageId, json weight)
            {
                if (const auto *entry = selection.catalog->find(imageId))
                {
                    const auto asset = previewAsset(*entry);
                    items.push_back({{"id", entry->id}, {"path", asset.available ? entry->path : ""},
                                     {"name", entry->name}, {"version", asset.version()},
                                     {"weight", std::move(weight)}});
                }
            };
            const int current = selection.imageIds[position];
            bool containsCurrent = false;
            for (const auto &candidate : row.candidates)
            {
                append(candidate.imageId, 1.0 / (1.0 + std::max(0.0, candidate.rankScore)));
                containsCurrent = containsCurrent || candidate.imageId == current;
            }
            if (!containsCurrent && current > 0)
            {
                append(current, nullptr);
            }
            return {{"ok", true}, {"imageId", current}, {"candidates", std::move(items)}};
        }
        throw std::invalid_argument("unknown editor session operation");
    }

private:
    MosaicRequest request;
    MosaicSelection selection;
    PreviewWorkspace workspace;
    std::array<FileStamp, 3> sources{};
    std::atomic_bool canceled{false};
    EditorFrame frame;
    std::unique_ptr<ImageCache> cache;
};
}

int runEditorSession(std::istream &input, std::ostream &output)
{
    EditorSession session;
    std::string line;
    while (std::getline(input, line))
    {
        try
        {
            if (line.size() > 16 * 1024 * 1024)
            {
                throw std::invalid_argument("editor request is too large");
            }
            const auto result = session.dispatch(json::parse(line));
            output << result.dump(-1, ' ', false, json::error_handler_t::replace) << std::endl;
        }
        catch (const std::exception &error)
        {
            output << json{{"ok", false}, {"message", error.what()}}.dump(-1, ' ', false, json::error_handler_t::replace)
                   << std::endl;
        }
    }
    return 0;
}
} // namespace mosaicraft
