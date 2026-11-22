#include "LibraryMaintenance.h"

#include "ApiJson.h"
#include "ApiRequestParser.h"
#include "BuildService.h"
#include "Database.h"
#include "DatabaseService.h"
#include "ImageNormalizer.h"
#include "LibraryAccess.h"
#include "ManagedLibrary.h"
#include "OutputPublication.h"
#include "ProjectService.h"
#include "UnicodeIO.h"
#include "third_party/picosha2/picosha2.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mosaicraft
{
using namespace managed_library;
using nlohmann::json;
namespace fs = std::filesystem;

namespace
{
constexpr char receiptFile[] = "desktop-library.json";

std::string activeGeneration(const fs::path &directory)
{
    std::ifstream file(directory / activeLibraryFile, std::ios::binary);
    char name[64];
    file.read(name, sizeof(name));
    const std::string result(name, static_cast<size_t>(file.gcount()));
    return isLibraryGeneration(result) ? result : std::string{};
}

json readJson(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    auto value = file ? json::parse(file, nullptr, false) : json{};
    return value.is_object() ? std::move(value) : json::object();
}

std::pair<std::string, size_t> sourceSnapshot(const fs::path &source, const fs::path &managed, const BuildRequest &request)
{
    std::vector<fs::path> images;
    const auto collect = [&](const auto &entries) {
        for (const auto &entry : entries)
        {
            if (entry.is_regular_file() && !ImageNormalizer::libraryOutputExtension(pathToUtf8(entry.path())).empty())
            {
                if (entry.is_symlink() && pathWithin(entry.path(), managed))
                    throw std::invalid_argument("a source image points into the generated library: " + pathToUtf8(entry.path()));
                images.push_back(entry.path());
            }
        }
    };
    if (request.recursive) collect(fs::recursive_directory_iterator(source));
    else collect(fs::directory_iterator(source));
    std::sort(images.begin(), images.end());

    const auto options = json::array({pathToUtf8(source), request.recursive, request.normalizeWidth,
        request.normalizeHeight, request.encoding.jpgQuality, request.encoding.pngLevel,
        request.encoding.webpQuality.value_or(0)}).dump();
    picosha2::hash256_one_by_one hash;
    hash.process(options.begin(), options.end());
    for (const auto &image : images)
    {
        const auto record = json::array({image.lexically_relative(source).generic_u8string(), sha256File(image)}).dump();
        hash.process(record.begin(), record.end());
    }
    hash.finish();
    return {picosha2::get_hash_hex_string(hash), images.size()};
}

void writeFile(const fs::path &path, const std::string &contents)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
    output.close();
    if (!output) throw std::runtime_error("cannot write " + pathToUtf8(path));
}

bool containsOnlyGeneratedFiles(const fs::path &directory)
{
    const auto marker = readJson(directory / ".mosaicraft-output.json");
    const auto entries = marker.find("files");
    if (entries == marker.end() || !entries->is_array()) return false;

    std::set<std::string> files{".mosaicraft-output.json", "mosaicraft.db.library.lock", "mosaicraft.db-wal",
        "mosaicraft.db-shm", "features/lib.ann196", "features/lib.ann196.meta", "features/lib.ann196.tmp",
        "features/lib.ann196.meta.tmp", "index/lib.ann196", "index/lib.ann196.meta", "index/lib.ann196.tmp",
        "index/lib.ann196.meta.tmp"};
    std::set<fs::path> directories{"index"};
    for (const auto &entry : *entries)
    {
        if (!entry.is_string()) return false;
        const auto &name = entry.get_ref<const std::string &>();
        files.insert(name);
        for (auto parent = u8path(name).parent_path(); !parent.empty(); parent = parent.parent_path())
        {
            if (parent.has_root_path()) return false;
            directories.insert(parent);
        }
    }
    for (const auto &entry : fs::recursive_directory_iterator(directory))
    {
        if (entry.is_symlink() || !pathWithin(entry.path(), directory)) return false;
        const auto relative = entry.path().lexically_relative(directory);
        if (entry.is_directory() ? !directories.count(relative) :
            !entry.is_regular_file() || !files.count(relative.generic_u8string())) return false;
    }
    return true;
}

void removeObsolete(const fs::path &directory, const std::string &active)
{
    try
    {
        for (const auto &generation : fs::directory_iterator(directory))
        {
            const auto path = generation.path();
            const auto name = pathToUtf8(path.filename());
            std::error_code error;
            if (name == active || !isLibraryGeneration(name) || !generation.is_directory(error) || error ||
                fs::canonical(path, error) != path || error) continue;
            const auto receipt = readJson(path / receiptFile);
            if (!receipt.contains("generation") || receipt["generation"] != name || !containsOnlyGeneratedFiles(path)) continue;

            const auto database = path / databaseFile;
            const auto lock = u8path(pathToUtf8(database) + ".library.lock");
            {
                const LibraryAccess access(pathToUtf8(database), true);
                if (!access.acquired()) continue;
                // A render started before publication may still be reading the old generation.
                for (const auto &entry : fs::directory_iterator(path))
                {
                    if (entry.path() != lock && pathWithin(entry.path(), path)) fs::remove_all(entry.path(), error);
                }
            }
            fs::remove(lock, error);
            fs::remove(path, error);
        }
    }
    catch (const fs::filesystem_error &)
    {
        // Cleanup can be retried on the next launch and does not affect the active generation.
    }
}

json verifiedLibrary(const fs::path &database, const DatabaseHealthResult &health, bool rebuilt)
{
    if (!health.status.ok) return serviceResultToJson(health.status);
    return {{"ok", true}, {"state", rebuilt ? "rebuilt" : "verified"}, {"empty", health.health.empty},
            {"database", pathToUtf8(database)}};
}
} // namespace

json maintainDesktopLibrary(const json &payload)
{
    BuildRequest request;
    RequestError error;
    if (!parseBuildRequestJson(payload.at("request").dump(), request, error))
        throw std::invalid_argument(error.message);

    const auto root = fs::weakly_canonical(fs::absolute(u8path(request.outputDir)));
    const auto directory = root / managedDirectory;
    if (fs::weakly_canonical(directory) != directory)
        throw std::invalid_argument("the managed library directory cannot be a link");
    // The only replaceable artifacts live inside the desktop's cache. A library supplied by the user stays usable.
    if (!request.inputDir.empty())
    {
        protectOutput(pathToUtf8(directory), {request.inputDir});
        fs::create_directories(directory);
    }

    const auto verify = [](const fs::path &db) { return DatabaseService().health({pathToUtf8(db), true}); };
    if (!fs::exists(directory))
        return verifiedLibrary(root / databaseFile, verify(root / databaseFile), false);

    const LibraryAccess maintenanceLock(pathToUtf8(directory / "maintenance"), true, 5000);
    if (!maintenanceLock.acquired()) throw std::runtime_error("another process is maintaining this library");
    const auto active = activeGeneration(directory);
    const auto activePath = directory / active;
    const auto database = (active.empty() ? root : activePath) / databaseFile;
    const auto health = verify(database);
    if (request.inputDir.empty()) return verifiedLibrary(database, health, false);

    const auto source = fs::canonical(u8path(request.inputDir));
    const auto snapshot = sourceSnapshot(source, directory, request);
    auto receipt = readJson(activePath / receiptFile);
    if (health.status.ok && !active.empty() && receipt["generation"] == active &&
        receipt["sourceHash"] == snapshot.first && receipt["records"] == health.health.total)
    {
        removeObsolete(directory, active);
        return verifiedLibrary(database, health, false);
    }

    std::random_device random;
    std::string generation;
    do { generation = std::string(generationPrefix) + std::to_string(random()); }
    while (fs::exists(directory / generation));
    OutputPublication publication(pathToUtf8(directory / generation), true, false, {pathToUtf8(source)});
    const auto stage = u8path(publication.workPath());
    const auto stagedDb = stage / databaseFile;
    RuntimeContext runtime(false);
    if (snapshot.second == 0)
    {
        Database empty(pathToUtf8(stagedDb), Database::OpenMode::Build);
        if (!empty.createTables(request.normalizeWidth, request.normalizeHeight))
            throw std::runtime_error("cannot create an empty material library");
    }
    else
    {
        request.inputDir = pathToUtf8(source);
        request.outputDir = publication.workPath();
        request.dbPath.clear();
        request.appendMode = request.normalizeOnly = request.forceMode = false;
        const auto built = BuildService().run(request, runtime);
        if (!built.ok) return serviceResultToJson(built);
        const auto visuals = runtime.snapshot().buildVisuals;
        if (!visuals || visuals->normalized != snapshot.second)
            throw std::runtime_error("one or more source images could not be normalized");
    }
    const auto rebuilt = verify(stagedDb);
    if (!rebuilt.status.ok) return serviceResultToJson(rebuilt.status);
    if (snapshot != sourceSnapshot(source, directory, request))
        throw std::runtime_error("the material source changed during the rebuild");

    writeFile(stage / receiptFile, json{{"generation", generation}, {"sourceHash", snapshot.first},
                                      {"records", rebuilt.health.total}}.dump());
    writeFile(stage / activeLibraryFile, generation);
    // The active pointer changes only after the complete library has been verified and moved into place.
    publication.commit(runtime, [&] {
        fs::rename(directory / generation / activeLibraryFile, directory / activeLibraryFile);
    });
    removeObsolete(directory, generation);
    return verifiedLibrary(directory / generation / databaseFile, rebuilt, true);
}
} // namespace mosaicraft
