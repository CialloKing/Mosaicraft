#include "EditorSession.h"
#include "EditorCandidates.h"
#include "core/ApiJson.h"
#include "core/ApiRequestParser.h"
#include "core/BrightnessAdjustment.h"
#include "core/CliOptions.h"
#include "core/ImageCache.h"
#include "core/MosaicPlanning.h"
#include "core/MosaicSelection.h"
#include "core/NumericParsing.h"
#include "core/UnicodeIO.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string_view>
#include <utility>

using namespace mosaicraft;
using nlohmann::json;

namespace
{
constexpr int previewEdge = 1800;

MosaicRequest parseEditorRequest(const json &arguments)
{
    const auto args = arguments.get<std::vector<std::string>>();
    if (args.size() < 2 || args[0] != "create")
    {
        throw std::invalid_argument("editor requires create arguments");
    }
    json fields{{"inputPath", args[1]}};
    const auto &options = cliOptions();
    for (size_t i = 2; i < args.size(); ++i)
    {
        const auto &key = args[i];
        const auto option = std::find_if(options.begin(), options.end(), [&](const CliOption &item) {
            return cliOptionSupports(item, "create") &&
                   (key == "--" + std::string(item.name) || (*item.shortName && key == "-" + std::string(item.shortName)));
        });
        if (option == options.end())
        {
            throw std::invalid_argument("unknown editor option: " + key);
        }
        const std::string_view type = option->type;
        if (type == "bool" && *option->field)
        {
            fields[option->field] = true;
            continue;
        }
        if (++i == args.size() || args[i].empty())
        {
            throw std::invalid_argument("missing value for " + key);
        }
        const auto &value = args[i];
        if (type == "size")
        {
            int width, height;
            if (!parseSize(value, width, height))
            {
                throw std::invalid_argument(key + " requires positive WxH");
            }
            if (key == "--match-tile")
            {
                fields["tileW"] = width;
                fields["tileH"] = height;
            }
            else if (key == "--output-tile")
            {
                fields["nativeTileW"] = width;
                fields["nativeTileH"] = height;
            }
        }
        else if (!*option->field)
        {
            throw std::invalid_argument("unsupported editor option: " + key);
        }
        else if (type == "int")
        {
            int number;
            if (!parseStrictInt(value, number)) throw std::invalid_argument("invalid number for " + key);
            fields[option->field] = number;
        }
        else if (type == "double")
        {
            auto number = json::parse(value, nullptr, false);
            if (!number.is_number() || !std::isfinite(number.get<double>()))
                throw std::invalid_argument("invalid number for " + key);
            fields[option->field] = std::move(number);
        }
        else if (type == "seed")
        {
            if (value.find_first_not_of("0123456789") != std::string::npos)
                throw std::invalid_argument("seed must be an unsigned 32-bit integer");
            fields[option->field] = std::stoull(value);
        }
        else
        {
            // 尺寸字符串交由和 CLI 相同的请求解析器校验，支持 auto 轴和百分比。
            fields[option->field] = value;
        }
    }
    if (fields.contains("colorStrength")) fields["colorAdjust"] = fields["colorStrength"].get<double>() != 0;
    MosaicRequest request;
    RequestError error;
    if (!parseMosaicRequestJson(fields.dump(), request, error)) throw std::invalid_argument(error.message);
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

std::string encodeBase64(const std::vector<unsigned char> &bytes)
{
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t offset = 0; offset < bytes.size(); offset += 3)
    {
        const unsigned first = bytes[offset];
        const unsigned second = offset + 1 < bytes.size() ? bytes[offset + 1] : 0;
        const unsigned third = offset + 2 < bytes.size() ? bytes[offset + 2] : 0;
        output += alphabet[first >> 2];
        output += alphabet[((first & 3) << 4) | (second >> 4)];
        output += offset + 1 < bytes.size() ? alphabet[((second & 15) << 2) | (third >> 6)] : '=';
        output += offset + 2 < bytes.size() ? alphabet[third & 63] : '=';
    }
    return output;
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

void replaceAsset(MosaicSelection &selection, const json &payload)
{
    const auto position = readPosition(payload, selection);
    const auto &value = payload.at("imageId");
    if (!value.is_number_unsigned() || value == 0 || value > INT_MAX)
    {
        throw std::invalid_argument("invalid replacement image ID");
    }
    const auto *entry = selection.catalog ? selection.catalog->find(value.get<int>()) : nullptr;
    const auto asset = entry ? previewAsset(*entry) : PreviewAsset{};
    if (!asset.available || payload.at("version") != asset.version())
    {
        throw std::invalid_argument("replacement image changed or is unavailable");
    }
    if (selection.indices[position] != entry->index)
    {
        selection.indices[position] = entry->index;
        selection.imageIds[position] = entry->id;
        selection.assets.insert_or_assign(entry->id, asset);
        ++selection.manualEdits;
    }
}

int boundary(int cell, int cells, int pixels)
{
    return static_cast<int>((int64_t(cell) * pixels + cells - 1) / cells);
}

struct EditorFrame
{
    cv::Mat image, reference;

    void paint(const MosaicSelection &selection, const MosaicEngine::Config &config, size_t position,
               ImageCache &cache)
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
                if (config.colorAdjust)
                {
                    detail::adjustBrightnessAt(tile, config.colorStrength, config.seed.value_or(0), position);
                }
                if (config.overlayStrength > 0)
                {
                    cv::addWeighted(tile, 1 - config.overlayStrength, reference(region), config.overlayStrength, 0, tile);
                }
                return;
            }
        }
        tile.setTo(cv::Scalar());
    }

    void compose(const MosaicSelection &selection, MosaicEngine::Config config, ImageCache &cache)
    {
        orientOutputTile(config, selection.featureWidth, selection.featureHeight);
        const double width = double(config.nativeTileW) * selection.columns;
        const double height = double(config.nativeTileH) * selection.rows;
        const double scale = std::min(1.0, previewEdge / std::max(width, height));
        const cv::Size size(std::max(1, cvRound(width * scale)), std::max(1, cvRound(height * scale)));
        image.create(size, CV_8UC3);
        cv::resize(cv::imdecode(selection.referenceJpeg, cv::IMREAD_COLOR), reference, size, 0, 0, cv::INTER_LINEAR);
        for (int top = 0; top < size.height;)
        {
            const int row = static_cast<int>(int64_t(top) * selection.rows / size.height);
            for (int left = 0; left < size.width;)
            {
                const int column = static_cast<int>(int64_t(left) * selection.columns / size.width);
                paint(selection, config, size_t(row) * selection.columns + column, cache);
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
        if (operation == "preview")
        {
            auto nextRequest = parseEditorRequest(payload.at("args"));
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
            nextFrame.compose(nextSelection, nextRequest.config, *nextCache);
            auto response = json{{"ok", true}, {"image", encodeImage(nextFrame.image)},
                                 {"reference", encodeBase64(nextSelection.referenceJpeg)}, {"seed", seed},
                                 {"columns", nextSelection.columns}, {"rows", nextSelection.rows},
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
            auto output = parseEditorRequest(payload.at("args"));
            output.config.seed = request.config.seed;
            RuntimeContext runtime(false);
            TaskResourceMonitor monitor(runtime, output.resourceMonitor);
            return serviceResultToJson(MosaicService().run(output, runtime, &selection));
        }
        const auto position = readPosition(payload, selection);
        if (operation == "replace")
        {
            replaceAsset(selection, payload);
            frame.paint(selection, request.config, position, *cache);
            return {{"ok", true}, {"image", encodeImage(frame.image)}, {"imageId", selection.imageIds[position]}};
        }
        if (operation == "candidates")
        {
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
            std::vector<EditorCandidate> candidates;
            const auto status = editorCandidates(request, selection, workspace, position, candidates);
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
            for (const auto &candidate : candidates)
            {
                append(candidate.imageId, 1.0 / (1.0 + std::max(0.0, candidate.score)));
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
