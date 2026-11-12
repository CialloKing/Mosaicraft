#include "OutputPublication.h"
#include "UnicodeIO.h"
#include "json.hpp"
#include <fstream>
#include <random>
#include <set>
#ifdef _WIN32
#include <windows.h>
#endif

namespace mosaicraft
{
namespace fs = std::filesystem;
using json = nlohmann::json;
static std::string portablePath(const fs::path &path)
{
    auto value = pathToUtf8(path);
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

bool pathWithin(const fs::path &path, const fs::path &root)
{
    const auto a = fs::weakly_canonical(path), b = fs::weakly_canonical(root);
    auto part = a.begin();
    for (const auto &component : b)
    {
        if (part == a.end())
        {
            return false;
        }
#ifdef _WIN32
        if (CompareStringOrdinal(part->c_str(), -1, component.c_str(), -1, TRUE) != CSTR_EQUAL)
#else
        if (*part != component)
#endif
        {
            return false;
        }
        ++part;
    }
    return true;
}

void protectOutput(const std::string &output, const std::vector<std::string> &inputs)
{
    const auto destination = u8path(output);
    for (const auto &input : inputs)
    {
        if (input.empty())
        {
            continue;
        }
        const auto source = u8path(input);
        if ((pathWithin(source, destination) && pathWithin(destination, source)) ||
            (fs::is_directory(source) && pathWithin(destination, source)) ||
            (fs::is_directory(destination) && pathWithin(source, destination)))
        {
            throw std::invalid_argument("output overlaps protected input: " + input);
        }
    }
}

static void verifyOwnedDirectory(const fs::path &root)
{
    std::set<std::string> allowed;
    const auto read = [&](const fs::path &file)
    {
        std::ifstream stream(file, std::ios::binary);
        if (!stream)
        {
            throw std::invalid_argument("output is not a recognized Mosaicraft directory");
        }
        return json::parse(stream);
    };
    if (fs::is_regular_file(root / "project.json"))
    {
        const auto manifest = read(root / "project.json");
        if (manifest.value("type", "") != "mosaicraft-project" || !manifest.at("resources").is_array())
        {
            throw std::invalid_argument("unrecognized project output");
        }
        allowed.insert("project.json");
        for (const auto &resource : manifest.at("resources"))
        {
            allowed.insert(resource.at("path").get<std::string>());
        }
    }
    else
    {
        const auto marker = read(root / ".mosaicraft-output.json");
        if (marker.value("type", "") != "mosaicraft-output")
        {
            throw std::invalid_argument("unrecognized output ownership marker");
        }
        allowed.insert(".mosaicraft-output.json");
        for (const auto &file : marker.at("files"))
        {
            allowed.insert(file.get<std::string>());
        }
    }
    for (const auto &entry : fs::recursive_directory_iterator(root))
    {
        if (entry.is_symlink() || !pathWithin(entry.path(), root))
        {
            throw std::invalid_argument("output contains a link or escaping path");
        }
        const auto relative = portablePath(entry.path().lexically_relative(root));
        if (entry.is_directory())
        {
            bool known = false;
            for (const auto &file : allowed)
            {
                known = known || file.rfind(relative + "/", 0) == 0;
            }
            if (known)
            {
                continue;
            }
        }
        else if (entry.is_regular_file() && allowed.count(relative))
        {
            continue;
        }
        throw std::invalid_argument("output contains an additional file: " + relative);
    }
}

static void markOwnedDirectory(const fs::path &root)
{
    if (fs::exists(root / "project.json"))
    {
        return;
    }
    json marker{{"type", "mosaicraft-output"}, {"version", 1}, {"files", json::array()}};
    for (const auto &entry : fs::recursive_directory_iterator(root))
    {
        if (entry.is_regular_file())
        {
            marker["files"].push_back(portablePath(entry.path().lexically_relative(root)));
        }
    }
    std::ofstream output(root / ".mosaicraft-output.json", std::ios::binary);
    output << marker.dump(2);
    output.close();
    if (!output)
    {
        throw std::runtime_error("cannot write output ownership marker");
    }
}

OutputPublication::OutputPublication(const std::string &path, bool isDirectory, bool allowOverwrite,
                                     std::vector<std::string> inputs)
    : destination(fs::absolute(u8path(path))), protectedInputs(std::move(inputs)), directory(isDirectory),
      overwrite(allowOverwrite)
{
    protectOutput(path, protectedInputs);
    if (fs::exists(destination))
    {
        if (!overwrite || fs::is_symlink(destination) || fs::is_directory(destination) != directory)
        {
            throw std::invalid_argument("output exists; use --overwrite for a compatible output");
        }
        if (directory)
        {
            verifyOwnedDirectory(destination);
        }
    }
    fs::create_directories(destination.parent_path());
    std::random_device random;
    do
    {
        staging = destination.parent_path() / (".mosaicraft-stage-" + std::to_string(random()));
    } while (!fs::create_directory(staging));
}

OutputPublication::~OutputPublication()
{
    std::error_code error;
    fs::remove_all(staging, error);
}

std::string OutputPublication::workPath() const
{
    return pathToUtf8(directory ? staging : staging / destination.filename());
}

void OutputPublication::commit(RuntimeContext &runtime, const std::function<void()> &beforeFinalize)
{
    std::vector<std::pair<fs::path, fs::path>> moves, backups;
    if (directory)
    {
        markOwnedDirectory(staging);
        moves.emplace_back(staging, destination);
    }
    else
    {
        for (const auto &entry : fs::directory_iterator(staging))
        {
            if (entry.is_directory())
            {
                // 单图分析报告也是独立副产物目录，覆盖时遵循同一所有权规则。
                markOwnedDirectory(entry.path());
            }
            moves.emplace_back(entry.path(), destination.parent_path() / entry.path().filename());
        }
    }
    size_t published = 0;
    try
    {
        for (const auto &[from, to] : moves)
        {
            // 编码策略可能调整后缀，因此发布时必须再次检查实际产物路径。
            protectOutput(pathToUtf8(to), protectedInputs);
            if (fs::exists(to))
            {
                if (!overwrite || fs::is_symlink(to) || fs::is_directory(to) != fs::is_directory(from))
                {
                    throw std::invalid_argument("output artifact already exists: " + pathToUtf8(to));
                }
                if (fs::is_directory(to))
                {
                    verifyOwnedDirectory(to);
                }
                auto backup = staging.parent_path() /
                              (pathToUtf8(staging.filename()) + "-backup-" + std::to_string(backups.size()));
                fs::rename(to, backup);
                backups.emplace_back(backup, to);
            }
            fs::rename(from, to);
            ++published;
        }
        if (beforeFinalize)
        {
            beforeFinalize();
        }
    }
    catch (...)
    {
        for (size_t i = published; i > 0; --i)
        {
            fs::rename(moves[i - 1].second, moves[i - 1].first);
        }
        for (auto it = backups.rbegin(); it != backups.rend(); ++it)
        {
            fs::rename(it->first, it->second);
        }
        throw;
    }
    runtime.relocateArtifacts(pathToUtf8(staging), pathToUtf8(directory ? destination : destination.parent_path()));
    committed = true;
    for (const auto &[backup, original] : backups)
    {
        std::error_code error;
        fs::remove_all(backup, error);
    }
}
} // namespace mosaicraft
