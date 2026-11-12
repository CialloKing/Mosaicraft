#include "ProjectService.h"
#include "ExecutionScope.h"
#include "OutputPublication.h"
#include "PortableViewer.h"
#include "RequestValidation.h"
#include "UnicodeIO.h"
#include "third_party/miniz/miniz.h"
#include "third_party/picosha2/picosha2.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <unordered_map>

namespace mosaicraft
{
namespace fs = std::filesystem;
using json = nlohmann::json;
namespace
{
constexpr uint64_t manifestLimit = 128ULL * 1024 * 1024;

fs::path resourcePath(const fs::path &root, const std::string &name)
{
    const auto relative = u8path(name);
    if (name.empty() || name.find('\\') != std::string::npos || name.find(':') != std::string::npos ||
        relative.is_absolute() || relative.has_root_path() || relative.lexically_normal() != relative)
    {
        throw std::invalid_argument("invalid project resource path: " + name);
    }
    for (const auto &part : relative)
    {
        if (part == ".." || part == ".")
        {
            throw std::invalid_argument("escaping project resource: " + name);
        }
    }
    const auto path = root / relative;
    if (!pathWithin(path, root) || fs::is_symlink(path))
    {
        throw std::invalid_argument("project resource escapes its directory: " + name);
    }
    return path;
}

struct Project
{
    fs::path root;
    json manifest;
};

Project loadProject(const std::string &input)
{
    auto path = fs::absolute(u8path(input));
    if (!fs::is_directory(path))
    {
        if (path.filename() != "project.json" && path.filename() != "index.html")
        {
            throw std::invalid_argument(
                "project input must be a directory, project.json or index.html; unpack ZIP first");
        }
        path = path.parent_path();
    }
    const auto manifestPath = resourcePath(path, "project.json");
    if (fs::file_size(manifestPath) > manifestLimit)
    {
        throw std::invalid_argument("project manifest exceeds 128 MiB");
    }
    std::ifstream inputFile(manifestPath, std::ios::binary);
    Project project{path, json::parse(inputFile)};
    const auto &m = project.manifest;
    if (m.at("type") != "mosaicraft-project" || m.at("version") != 1)
    {
        throw std::invalid_argument("unsupported Mosaicraft project version");
    }
    if (!m.at("version").is_number_integer() || !m.at("grid").at("columns").is_number_integer() ||
        !m.at("grid").at("rows").is_number_integer())
    {
        throw std::invalid_argument("project version and grid dimensions must be integers");
    }
    const int64_t columns = m.at("grid").at("columns").get<int64_t>();
    const int64_t rows = m.at("grid").at("rows").get<int64_t>();
    if (columns <= 0 || rows <= 0 || columns > 1000000 || rows > 1000000 || columns * rows > 100000000 ||
        m.at("layout").size() != size_t(columns * rows) || !m.at("layout").is_array() || !m.at("assets").is_array() ||
        !m.at("resources").is_array())
    {
        throw std::invalid_argument("invalid project grid or layout");
    }
    std::set<std::string> resources;
    std::unordered_map<std::string, const json *> resourceTable;
    for (const auto &resource : m.at("resources"))
    {
        const auto name = resource.at("path").get<std::string>();
        resourcePath(path, name);
        const auto hash = resource.at("sha256").get<std::string>();
        if (name == "project.json" || !resources.insert(name).second || hash.size() != 64 ||
            hash.find_first_not_of("0123456789abcdef") != std::string::npos ||
            !resource.at("bytes").is_number_unsigned())
        {
            throw std::invalid_argument("invalid or repeated project resource: " + name);
        }
        resourceTable[name] = &resource;
        const auto &width = resource.at("width"), &height = resource.at("height");
        if (!width.is_number_integer() || !height.is_number_integer() ||
            !((width == 0 && height == 0) || validImageSize(width.get<int64_t>(), height.get<int64_t>())))
        {
            throw std::invalid_argument("invalid project resource dimensions: " + name);
        }
    }
    const auto requireResource = [&](const std::string &name)
    {
        if (!resources.count(name))
        {
            throw std::invalid_argument("unlisted project resource: " + name);
        }
    };
    requireResource(m.at("target"));
    requireResource(m.at("overview"));
    requireResource(m.at("entry"));
    requireResource(m.at("targetDisplay"));
    for (const auto &asset : m.at("assets"))
    {
        for (const auto &key : {"path", "display", "thumbnail32", "thumbnail128"})
        {
            requireResource(asset.at(key));
        }
        const auto &resource = *resourceTable.at(asset.at("path").get<std::string>());
        if (asset.at("sha256") != resource.at("sha256") || asset.at("width") != resource.at("width") ||
            asset.at("height") != resource.at("height") || !asset.at("width").is_number_integer() ||
            !asset.at("height").is_number_integer() ||
            !validImageSize(asset.at("width").get<int64_t>(), asset.at("height").get<int64_t>()))
        {
            throw std::invalid_argument("project asset metadata differs from its resource");
        }
    }
    for (const auto &cell : m.at("layout"))
    {
        if (!cell.is_number_integer() || cell.get<int64_t>() < -1 ||
            cell.get<int64_t>() >= static_cast<int64_t>(m.at("assets").size()))
        {
            throw std::invalid_argument("project layout contains an invalid asset index");
        }
    }
    for (const auto &dimensions : {m.at("outputTile"), m.at("grid").at("matchTile")})
    {
        if (!dimensions.is_array() || dimensions.size() != 2 || !dimensions[0].is_number_integer() ||
            !dimensions[1].is_number_integer() ||
            !validImageSize(dimensions[0].get<int64_t>(), dimensions[1].get<int64_t>()))
        {
            throw std::invalid_argument("invalid project tile dimensions");
        }
    }
    const auto tile = m.at("outputTile").get<std::vector<int>>();
    const auto match = m.at("grid").at("matchTile").get<std::vector<int>>();
    const double blend = m.at("appearance").at("targetBlend");
    const double jitter = m.at("appearance").at("brightnessJitter");
    const double seed = m.at("seed").get<double>();
    if (tile.size() != 2 || match.size() != 2 || !validImageSize(tile[0], tile[1]) ||
        !validImageSize(match[0], match[1]) || !std::isfinite(blend) || blend < 0 || blend > 1 || jitter != 0 ||
        !m.at("seed").is_number_integer() || seed < 0 || seed > UINT32_MAX)
    {
        throw std::invalid_argument("invalid project appearance, seed or dimensions");
    }
    const auto &targetResource = *resourceTable.at(m.at("target").get<std::string>());
    if (targetResource.at("width") != columns * match[0] || targetResource.at("height") != rows * match[1] ||
        !validImageSize(columns * match[0], rows * match[1]))
    {
        throw std::invalid_argument("prepared target dimensions do not match project grid");
    }
    return project;
}

json browseSnapshot(const json &manifest)
{
    json snapshot;
    for (const auto &key :
         {"version", "grid", "outputTile", "layout", "assets", "appearance", "overview", "targetDisplay"})
    {
        snapshot[key] = manifest.at(key);
    }
    return snapshot;
}

void verifyResource(const Project &project, const json &resource, bool deep)
{
    const auto name = resource.at("path").get<std::string>();
    const auto path = resourcePath(project.root, name);
    if (!fs::is_regular_file(path) || fs::file_size(path) != resource.at("bytes").get<uint64_t>() ||
        sha256File(path) != resource.at("sha256").get<std::string>())
    {
        throw std::runtime_error("project resource missing, truncated or checksum mismatch: " + name);
    }
    if (deep && resource.value("width", 0) > 0)
    {
        const auto image = imreadUnicode(pathToUtf8(path));
        if (image.empty() || image.cols != resource.at("width") || image.rows != resource.at("height"))
        {
            throw std::runtime_error("project resource decode or dimension mismatch: " + name);
        }
    }
}

void verifySnapshot(const Project &project)
{
    const auto bytes = readBytesUnicode(pathToUtf8(resourcePath(project.root, project.manifest.at("entry"))));
    const std::string html(bytes.begin(), bytes.end());
    const std::string start = "<script id=\"mosaicraft-snapshot\" type=\"application/json\">";
    const auto position = html.find(start);
    const auto end = html.find("</script>", position == std::string::npos ? 0 : position + start.size());
    if (position == std::string::npos || end == std::string::npos ||
        json::parse(html.substr(position + start.size(), end - position - start.size())) !=
            browseSnapshot(project.manifest))
    {
        throw std::runtime_error("HTML browse snapshot differs from project.json");
    }
}

void saveText(const fs::path &path, const std::string &text)
{
    std::ofstream output(path, std::ios::binary);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    if (!output)
    {
        throw std::runtime_error("cannot write project resource: " + pathToUtf8(path));
    }
}

void saveImage(const fs::path &path, const cv::Mat &image, int longest = 0)
{
    cv::Mat display = image;
    if (longest > 0 && std::max(image.cols, image.rows) > longest)
    {
        const double scale = double(longest) / std::max(image.cols, image.rows);
        cv::resize(image, display, {std::max(1, int(image.cols * scale)), std::max(1, int(image.rows * scale))}, 0, 0,
                   cv::INTER_AREA);
    }
    if (!imwriteUnicode(pathToUtf8(path), display,
                        path.extension() == ".png" ? std::vector<int>{cv::IMWRITE_PNG_COMPRESSION, 1}
                                                   : std::vector<int>{cv::IMWRITE_JPEG_QUALITY, 90}))
    {
        throw std::runtime_error("cannot encode project resource: " + pathToUtf8(path));
    }
}
} // namespace

std::string sha256File(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("cannot read resource: " + pathToUtf8(path));
    }
    picosha2::hash256_one_by_one hash;
    std::array<char, 65536> buffer;
    while (input)
    {
        input.read(buffer.data(), buffer.size());
        hash.process(buffer.begin(), buffer.begin() + input.gcount());
    }
    if (!input.eof())
    {
        throw std::runtime_error("resource read failed: " + pathToUtf8(path));
    }
    hash.finish();
    return picosha2::get_hash_hex_string(hash);
}

bool writeHtmlProject(const MosaicEngine::Config &config, const std::string &directory, int columns, int rows,
                      const std::vector<ImageRecord> &library, const std::vector<int> &selection, const cv::Mat &target,
                      OutputStats &stats, RuntimeContext &runtime)
{
    try
    {
        if ((config.colorAdjust && config.colorStrength != 0) || target.empty())
        {
            throw std::invalid_argument("HTML requires a prepared target and does not support brightness jitter");
        }
        runtime.stage("exporting_project");
        const auto root = u8path(directory);
        fs::create_directories(root / "assets");
        fs::create_directories(root / "thumbnails");
        json manifest{
            {"type", "mosaicraft-project"},
            {"version", 1},
            {"grid",
             {{"columns", columns}, {"rows", rows}, {"matchTile", {target.cols / columns, target.rows / rows}}}},
            {"outputTile", {config.nativeTileW, config.nativeTileH}},
            {"seed", config.seed.value_or(0)},
            {"appearance", {{"targetBlend", config.overlayStrength}, {"brightnessJitter", 0}}},
            {"entry", "index.html"},
            {"target", "target.png"},
            {"targetDisplay", "thumbnails/target.jpg"},
            {"overview", "overview.jpg"},
            {"assets", json::array()},
            {"layout", json::array()},
            {"resources", json::array()}};
        std::unordered_map<std::string, int> hashes;
        std::unordered_map<int, int> remap;
        const auto addResource = [&](const std::string &name, const cv::Size &dimensions = cv::Size())
        {
            const auto path = resourcePath(root, name);
            manifest["resources"].push_back({{"path", name},
                                             {"bytes", fs::file_size(path)},
                                             {"sha256", sha256File(path)},
                                             {"width", dimensions.width},
                                             {"height", dimensions.height}});
        };
        size_t processed = 0;
        for (int index : selection)
        {
            if (index < 0)
            {
                manifest["layout"].push_back(-1);
                continue;
            }
            if (size_t(index) >= library.size())
            {
                throw std::invalid_argument("selection asset index out of range");
            }
            if (!remap.count(index))
            {
                const auto source = u8path(library.at(index).filePath);
                const auto hash = sha256File(source);
                if (!hashes.count(hash))
                {
                    const int id = static_cast<int>(manifest["assets"].size());
                    auto extension = pathToUtf8(source.extension());
                    std::transform(extension.begin(), extension.end(), extension.begin(),
                                   [](unsigned char c)
                                   {
                                       return std::tolower(c);
                                   });
                    const std::string original = "assets/" + hash + extension;
                    fs::copy_file(source, resourcePath(root, original));
                    if (sha256File(root / u8path(original)) != hash)
                    {
                        throw std::runtime_error("asset changed while being copied");
                    }
                    const auto image = imreadUnicode(pathToUtf8(root / u8path(original)));
                    if (image.empty())
                    {
                        throw std::runtime_error("selected asset cannot be decoded: " + pathToUtf8(source));
                    }
                    const auto raw = imreadUnicode(pathToUtf8(root / u8path(original)), cv::IMREAD_UNCHANGED);
                    std::string display = original;
                    if ((extension != ".jpg" && extension != ".jpeg" && extension != ".png" && extension != ".webp") ||
                        raw.type() != CV_8UC3)
                    {
                        display = "thumbnails/" + hash + "-display.jpg";
                        saveImage(root / u8path(display), image);
                        addResource(display, image.size());
                    }
                    json asset{{"path", original},
                               {"sha256", hash},
                               {"width", image.cols},
                               {"height", image.rows},
                               {"display", display}};
                    addResource(original, image.size());
                    for (int size : {32, 128})
                    {
                        const auto name = "thumbnails/" + hash + "-" + std::to_string(size) + ".jpg";
                        saveImage(root / u8path(name), image, size);
                        const auto thumb = imreadUnicode(pathToUtf8(root / u8path(name)));
                        addResource(name, thumb.size());
                        asset["thumbnail" + std::to_string(size)] = name;
                    }
                    hashes.emplace(hash, id);
                    manifest["assets"].push_back(std::move(asset));
                    runtime.advance("assets", ++processed, -1, "assets");
                }
                remap[index] = hashes.at(hash);
            }
            manifest["layout"].push_back(remap.at(index));
        }
        saveImage(root / "target.png", target);
        addResource("target.png", target.size());
        saveImage(root / "thumbnails/target.jpg", target, 1400);
        addResource("thumbnails/target.jpg", imreadUnicode(pathToUtf8(root / "thumbnails/target.jpg")).size());

        const double ratio = double(columns) * config.nativeTileW / (double(rows) * config.nativeTileH);
        const int width = std::max(1, int(1600 * std::min(1.0, ratio)));
        const int height = std::max(1, int(1600 / std::max(1.0, ratio)));
        cv::Mat overview(height, width, CV_8UC3, cv::Scalar(64, 64, 64));
        // 总览缓存有明确预算，素材数增长时仍不分配最终尺寸的合成图。
        std::unordered_map<int, cv::Mat> thumbs;
        size_t thumbBytes = 0;
        for (size_t i = 0; i < selection.size(); ++i)
        {
            const int asset = manifest["layout"][i];
            const int x0 = int((i % columns) * uint64_t(width) / columns);
            const int x1 = int((i % columns + 1) * uint64_t(width) / columns);
            const int y0 = int((i / columns) * uint64_t(height) / rows);
            const int y1 = int((i / columns + 1) * uint64_t(height) / rows);
            if (asset < 0 || x0 == x1 || y0 == y1)
            {
                continue;
            }
            if (!thumbs.count(asset))
            {
                const auto thumb = imreadUnicode(pathToUtf8(root / u8path(manifest["assets"][asset]["thumbnail32"])));
                if (thumbBytes + thumb.total() * thumb.elemSize() > 16 * 1024 * 1024)
                {
                    thumbs.clear();
                    thumbBytes = 0;
                }
                thumbs[asset] = thumb;
                thumbBytes += thumb.total() * thumb.elemSize();
            }
            cv::resize(thumbs.at(asset), overview(cv::Rect(x0, y0, x1 - x0, y1 - y0)), {x1 - x0, y1 - y0});
        }
        if (config.overlayStrength > 0)
        {
            cv::Mat reference;
            cv::resize(target, reference, overview.size());
            cv::addWeighted(overview, 1 - config.overlayStrength, reference, config.overlayStrength, 0, overview);
        }
        saveImage(root / "overview.jpg", overview);
        addResource("overview.jpg", overview.size());
        auto snapshot = browseSnapshot(manifest).dump(-1, ' ', true);
        // JSON 放入 script 文本时必须转义 <，素材名不能提前结束元素。
        size_t at = 0;
        while ((at = snapshot.find('<', at)) != std::string::npos)
        {
            snapshot.replace(at, 1, "\\u003c");
            at += 6;
        }
        saveText(root / "index.html", std::string(portableViewerHead) + snapshot + portableViewerTail);
        addResource("index.html");
        saveText(root / "project.json", manifest.dump(2));
        stats.path = pathToUtf8(root / "index.html");
        stats.failed = static_cast<int>(std::count(selection.begin(), selection.end(), -1));
        stats.matched = static_cast<int>(selection.size()) - stats.failed;
        runtime.metric("projectAssetCount", hashes.size());
        uint64_t bytes = fs::file_size(root / "project.json");
        for (const auto &resource : manifest["resources"])
        {
            bytes += resource["bytes"].get<uint64_t>();
        }
        runtime.metric("projectBytes", double(bytes));
        runtime.output(int64_t(columns) * config.nativeTileW, int64_t(rows) * config.nativeTileH, "html", stats.matched,
                       stats.failed);
        runtime.artifact("project", "directory", directory);
        runtime.artifact("main", "project-entry", stats.path, "text/html");
        runtime.artifact("manifest", "project-manifest", pathToUtf8(root / "project.json"), "application/json");
        return true;
    }
    catch (const std::exception &error)
    {
        return runtime.fail("project_export_failed", error.what());
    }
}

void finalizeHtmlProject(const std::string &directory, RuntimeContext &runtime)
{
    auto project = loadProject(directory);
    auto &manifest = project.manifest;
    std::set<std::string> known{"project.json"};
    for (const auto &resource : manifest["resources"])
    {
        known.insert(resource["path"]);
    }
    // 可选分析在主输出之后生成，其副产物也必须进入完整交付和覆盖的资源边界。
    for (const auto &entry : fs::recursive_directory_iterator(project.root))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        auto relative = pathToUtf8(entry.path().lexically_relative(project.root));
        std::replace(relative.begin(), relative.end(), '\\', '/');
        if (known.insert(relative).second)
        {
            manifest["resources"].push_back({{"path", relative},
                                             {"bytes", fs::file_size(entry.path())},
                                             {"sha256", sha256File(entry.path())},
                                             {"width", 0},
                                             {"height", 0}});
        }
    }
    saveText(project.root / "project.json", manifest.dump(2));
    uint64_t bytes = fs::file_size(project.root / "project.json");
    for (const auto &resource : manifest["resources"])
    {
        bytes += resource["bytes"].get<uint64_t>();
    }
    runtime.metric("projectBytes", double(bytes));
    runtime.artifact("manifest", "project-manifest", pathToUtf8(project.root / "project.json"), "application/json");
}

json ProjectService::info(const std::string &input) const
{
    const auto project = loadProject(input);
    const auto &manifest = project.manifest;
    uint64_t bytes = fs::file_size(project.root / "project.json");
    for (const auto &resource : manifest["resources"])
    {
        bytes += resource["bytes"].get<uint64_t>();
    }
    return {{"path", pathToUtf8(project.root)},
            {"version", 1},
            {"grid", manifest["grid"]},
            {"outputTile", manifest["outputTile"]},
            {"assetCount", manifest["assets"].size()},
            {"appearance", manifest["appearance"]},
            {"seed", manifest["seed"]},
            {"estimatedBytes", bytes}};
}

ServiceResult ProjectService::check(const ProjectRequest &request, RuntimeContext &runtime) const
{
    try
    {
        const auto project = loadProject(request.projectPath);
        runtime.stage("checking_project");
        size_t count = 0;
        for (const auto &resource : project.manifest["resources"])
        {
            verifyResource(project, resource, request.deep);
            runtime.advance("resources", ++count, project.manifest["resources"].size(), "files");
        }
        verifySnapshot(project);
        return runtimeResult(ServiceResult::success("project integrity verified"), runtime);
    }
    catch (const std::exception &error)
    {
        return runtimeResult(runtimeFailure(runtime, 1, error.what(), "project_integrity_failed"), runtime);
    }
}

ServiceResult ProjectService::pack(const ProjectRequest &request, RuntimeContext &runtime) const
{
    try
    {
        const auto project = loadProject(request.projectPath);
        auto status = check(request, runtime);
        if (!status.ok)
        {
            return status;
        }
        protectOutput(request.outputPath, {pathToUtf8(project.root)});
        OutputPublication publication(request.outputPath, false, request.overwrite, {pathToUtf8(project.root)});
        std::ofstream output(u8path(publication.workPath()), std::ios::binary);
        mz_zip_archive zip{};
        zip.m_pIO_opaque = &output;
        zip.m_pWrite = [](void *opaque, mz_uint64 offset, const void *buffer, size_t size) -> size_t
        {
            auto &stream = *static_cast<std::ofstream *>(opaque);
            stream.seekp(static_cast<std::streamoff>(offset));
            stream.write(static_cast<const char *>(buffer), static_cast<std::streamsize>(size));
            return stream ? size : 0;
        };
        if (!mz_zip_writer_init_v2(&zip, 0, MZ_ZIP_FLAG_WRITE_ZIP64))
        {
            throw std::runtime_error("cannot initialize ZIP64 writer");
        }
        struct ZipGuard
        {
            mz_zip_archive &zip;
            ~ZipGuard()
            {
                mz_zip_writer_end(&zip);
            }
        } guard{zip};
        std::vector<std::string> names{"project.json"};
        std::unordered_map<std::string, const json *> resourceTable;
        for (const auto &resource : project.manifest["resources"])
        {
            names.push_back(resource["path"]);
            resourceTable[resource["path"].get<std::string>()] = &resource;
        }
        const auto top = pathToUtf8(project.root.filename());
        runtime.stage("packing_project");
        size_t completed = 0;
        for (const auto &name : names)
        {
            const auto path = resourcePath(project.root, name);
            std::ifstream input(path, std::ios::binary);
            const auto read = [](void *opaque, mz_uint64 offset, void *buffer, size_t size) -> size_t
            {
                auto &stream = *static_cast<std::ifstream *>(opaque);
                stream.clear();
                stream.seekg(static_cast<std::streamoff>(offset));
                stream.read(static_cast<char *>(buffer), static_cast<std::streamsize>(size));
                return static_cast<size_t>(stream.gcount());
            };
            if (!mz_zip_writer_add_read_buf_callback(&zip, (top + "/" + name).c_str(), read, &input,
                                                     fs::file_size(path), nullptr, nullptr, 0, MZ_NO_COMPRESSION,
                                                     nullptr, 0, nullptr, 0))
            {
                throw std::runtime_error("cannot archive project resource: " + name);
            }
            if (name == "project.json")
            {
                std::ifstream current(path, std::ios::binary);
                if (json::parse(current) != project.manifest)
                {
                    throw std::runtime_error("project manifest changed during packing");
                }
            }
            else
            {
                verifyResource(project, *resourceTable.at(name), false);
            }
            runtime.advance("resources", ++completed, names.size(), "files");
        }
        if (!mz_zip_writer_finalize_archive(&zip))
        {
            throw std::runtime_error("cannot finalize ZIP64 archive");
        }
        output.close();
        if (!output)
        {
            throw std::runtime_error("ZIP64 write failed");
        }
        runtime.artifact("archive", "project-archive", publication.workPath(), "application/zip");
        publication.commit(runtime);
        return runtimeResult(ServiceResult::success("project packed"), runtime);
    }
    catch (const std::exception &error)
    {
        runtime.discardArtifacts();
        return runtimeResult(runtimeFailure(runtime, 1, error.what(), "project_pack_failed"), runtime);
    }
}

ServiceResult ProjectService::render(const ProjectRequest &request, RuntimeContext &runtime) const
{
    try
    {
        std::lock_guard<std::mutex> lock(pipelineMutex());
        ExecutionScope scope;
        const auto project = loadProject(request.projectPath);
        auto config = request.config;
        const auto &manifest = project.manifest;
        if (!request.outputTileExplicit)
        {
            config.nativeTileW = manifest["outputTile"][0];
            config.nativeTileH = manifest["outputTile"][1];
        }
        if (!request.blendExplicit)
        {
            config.overlayStrength = manifest["appearance"]["targetBlend"];
        }
        if (!request.jitterExplicit)
        {
            config.colorStrength = manifest["appearance"]["brightnessJitter"];
            config.colorAdjust = config.colorStrength > 0;
        }
        config.seed = config.seed.value_or(manifest["seed"].get<uint32_t>());
        MosaicRequest validation;
        validation.inputPath = "target.png";
        validation.outputPath = request.outputPath;
        validation.config = config;
        std::string error;
        if (!validateMosaicRequest(validation, error))
        {
            throw std::invalid_argument(error);
        }
        config = validation.config;
        std::set<std::string> required{manifest["target"]};
        std::vector<ImageRecord> assets;
        for (const auto &asset : manifest["assets"])
        {
            const auto name = asset.at("path").get<std::string>();
            required.insert(name);
            ImageRecord record;
            record.id = static_cast<int>(assets.size()) + 1;
            record.filePath = pathToUtf8(resourcePath(project.root, name));
            assets.push_back(std::move(record));
        }
        runtime.stage("validating_project");
        for (const auto &resource : manifest["resources"])
        {
            if (required.count(resource["path"]))
            {
                verifyResource(project, resource, true);
            }
        }
        const auto target = imreadUnicode(pathToUtf8(resourcePath(project.root, manifest["target"])));
        const int columns = manifest["grid"]["columns"], rows = manifest["grid"]["rows"];
        if (target.cols != int64_t(columns) * manifest["grid"]["matchTile"][0].get<int>() ||
            target.rows != int64_t(rows) * manifest["grid"]["matchTile"][1].get<int>())
        {
            throw std::invalid_argument("prepared target dimensions do not match project grid");
        }
        const auto selection = manifest["layout"].get<std::vector<int>>();
        protectOutput(request.outputPath, {pathToUtf8(project.root)});
        OutputPublication publication(request.outputPath, config.outputMode != OutputMode::Image, request.overwrite,
                                      {pathToUtf8(project.root)});
        const auto path = publication.workPath();
        OutputStats stats;
        runtime.metric("seed", *config.seed);
        runtime.backends("none", "none");
        const bool ok = config.outputMode == OutputMode::Html
                            ? writeHtmlProject(config, path, columns, rows, assets, selection, target, stats, runtime)
                            : writeIndexedMosaicOutput(
                                  config, config.outputMode == OutputMode::Image ? path : path + "/index", columns,
                                  rows, config.nativeTileW, config.nativeTileH, assets, selection,
                                  [](cv::Mat &, double)
                                  {
                                  },
                                  stats, target, &runtime);
        if (!ok)
        {
            runtime.discardArtifacts();
            return runtimeResult(ServiceResult::failure(1, "project rendering failed"), runtime);
        }
        publication.commit(runtime);
        return runtimeResult(ServiceResult::success("project rendered without matching"), runtime);
    }
    catch (const std::exception &error)
    {
        runtime.discardArtifacts();
        return runtimeResult(runtimeFailure(runtime, 1, error.what(), "project_render_failed"), runtime);
    }
}
} // namespace mosaicraft
