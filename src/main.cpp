#include "core/ApiJson.h"
#include "core/ApiRequestParser.h"
#include "core/CliOptions.h"
#include "core/ConsoleGuard.h"
#include "core/LibraryVisuals.h"
#include "core/NumericParsing.h"
#include "core/ProjectService.h"
#include "core/RequestValidation.h"
#include "core/UnicodeIO.h"
#include "core/Version.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>

using namespace mosaicraft;
using json = nlohmann::json;
namespace
{
struct Arguments
{
    std::string group, command, input;
    std::map<std::string, std::string> values;
    bool has(const std::string &name) const
    {
        return values.count(name) != 0;
    }
    std::string get(const std::string &name, const std::string &fallback = {}) const
    {
        const auto found = values.find(name);
        return found == values.end() ? fallback : found->second;
    }
};

void help(const std::string &group, const std::string &command, bool all)
{
    std::cout << "Mosaicraft " << kVersion << "\n";
    if (command.empty())
    {
        std::cout << "Usage: mosaicraft <command> [options]\n"
                     "  create <target>            Match a target and create an output\n"
                     "  render <project>           Render a saved project without matching\n"
                     "  library build <source>     Create a new library\n"
                     "  library add <source>       Append using the library's existing size\n"
                     "  library report <library>   Statistics; --check / --deep for integrity\n"
                     "  library usage <library>    Recorded targets and cumulative tile counts\n"
                     "  library export <library>   Copy used, unused or all images\n"
                     "  library purge <library>    Report missing records; --apply removes them\n"
                     "  library normalize <source> Normalize and copy images without a database\n"
                     "  project info <project>     Read project metadata\n"
                     "  project check <project>    Verify resources and browse snapshot\n"
                     "  project pack <project>     Store a complete project as ZIP64\n"
                     "  inspect <target>           Image features and grid coverage\n"
                     "  devices --json             Enumerate compute devices and capabilities\n"
                     "Use '<command> --help' or '--help-all'. Web UI: MosaicraftWebUI.\n";
        return;
    }
    std::cout << "Usage: mosaicraft " << (group.empty() ? "" : group + " ") << command
              << (command == "devices" ? " [options]\n" : " <input> [options]\n");
    if (command == "render" || group == "project")
    {
        std::cout << "Project input: directory, project.json or index.html. Unpack ZIP before loading.\n";
    }
    for (const auto &option : cliOptions())
    {
        if (cliOptionSupports(option, command) && (all || !option.advanced))
        {
            std::string name = "  --" + std::string(option.name);
            if (*option.shortName)
            {
                name += ", -" + std::string(option.shortName);
            }
            if (std::string(option.type) != "bool")
            {
                name += " <value>";
            }
            std::cout << std::left << std::setw(31) << name << option.help << '\n';
        }
    }
    if (!all)
    {
        std::cout << "Use --help-all for advanced options.\n";
    }
    std::cout << "Exit codes: 0 success; 1 arguments/I/O; 2 library; 3 memory; 4 GPU.\n";
}

Arguments parse(const std::vector<std::string> &argv)
{
    Arguments args;
    size_t i = 1;
    if (i >= argv.size())
    {
        return args;
    }
    if (argv[i] == "--help" || argv[i] == "-h" || argv[i] == "--help-all")
    {
        args.values[argv[i] == "--help-all" ? "help-all" : "help"] = "true";
        return args;
    }
    args.command = argv[i++];
    if (args.command == "library" || args.command == "project")
    {
        args.group = args.command;
        args.command.clear();
        if (i == argv.size() || argv[i] == "--help" || argv[i] == "--help-all" || argv[i] == "-h")
        {
            args.values["help"] = "true";
            return args;
        }
        args.command = argv[i++];
    }
    const std::string commands = args.group == "library"   ? " build add report usage export purge normalize "
                                 : args.group == "project" ? " info check pack "
                                                           : " create render inspect devices ";
    if (commands.find(" " + args.command + " ") == std::string::npos)
    {
        throw std::invalid_argument("unknown command: " + args.command);
    }
    bool positional = false;
    for (; i < argv.size(); ++i)
    {
        auto token = argv[i];
        if (token == "--" && !positional)
        {
            positional = true;
            continue;
        }
        if (positional || token.empty() || token.front() != '-')
        {
            if (!args.input.empty())
            {
                throw std::invalid_argument("unexpected positional argument: " + token);
            }
            args.input = token;
            continue;
        }
        auto equal = token.find('=');
        const auto key = token.substr(0, equal);
        const CliOption *definition = nullptr;
        for (const auto &option : cliOptions())
        {
            if (key == "--" + std::string(option.name) ||
                (*option.shortName && key == "-" + std::string(option.shortName)))
            {
                definition = &option;
                break;
            }
        }
        if (!definition || !cliOptionSupports(*definition, args.command))
        {
            throw std::invalid_argument("unknown or inapplicable option for " + args.command + ": " + key);
        }
        std::string value = "true";
        if (std::string(definition->type) != "bool")
        {
            if (equal != std::string::npos)
            {
                value = token.substr(equal + 1);
            }
            else if (i + 1 < argv.size() && argv[i + 1].rfind("--", 0) != 0)
            {
                value = argv[++i];
            }
            else
            {
                throw std::invalid_argument("missing value for " + key);
            }
            if (value.empty())
            {
                throw std::invalid_argument("empty value for " + key);
            }
        }
        else if (equal != std::string::npos)
        {
            throw std::invalid_argument("flag does not accept a value: " + key);
        }
        if (!args.values.emplace(definition->name, value).second)
        {
            throw std::invalid_argument("repeated option: " + key);
        }
    }
    return args;
}

int integer(const std::string &value)
{
    int number;
    if (!parseStrictInt(value, number))
    {
        throw std::invalid_argument("invalid integer: " + value);
    }
    return number;
}

json requestFields(const Arguments &args)
{
    json fields = json::object();
    for (const auto &option : cliOptions())
    {
        if (!args.has(option.name) || !*option.field)
        {
            continue;
        }
        const auto value = args.get(option.name);
        const std::string type = option.type;
        if (type == "bool")
        {
            fields[option.field] = true;
        }
        else if (type == "int")
        {
            fields[option.field] = integer(value);
        }
        else if (type == "double")
        {
            char *end = nullptr;
            errno = 0;
            const auto number = std::strtod(value.c_str(), &end);
            if (errno || end == value.c_str() || *end || !std::isfinite(number))
            {
                throw std::invalid_argument("invalid number: " + value);
            }
            fields[option.field] = number;
        }
        else if (type == "seed")
        {
            if (value.find_first_not_of("0123456789") != std::string::npos)
            {
                throw std::invalid_argument("seed must be an unsigned 32-bit integer");
            }
            const auto seed = std::stoull(value);
            if (seed > UINT32_MAX)
            {
                throw std::invalid_argument("seed exceeds unsigned 32-bit range");
            }
            fields[option.field] = seed;
        }
        else
        {
            fields[option.field] = value;
        }
    }
    for (const auto &name : {"match-tile", "output-tile", "target-size"})
    {
        if (!args.has(name))
        {
            continue;
        }
        auto value = args.get(name);
        int width = 0, height = 0;
        const bool target = std::string(name) == "target-size";
        const auto split = value.find_first_of("xX");
        if (target && split != std::string::npos)
        {
            const auto w = value.substr(0, split), h = value.substr(split + 1);
            width = w == "auto" ? 0 : integer(w);
            height = h == "auto" ? 0 : integer(h);
            if ((width == 0 && w != "auto") || (height == 0 && h != "auto") || (!width && !height) || width < 0 ||
                height < 0)
            {
                throw std::invalid_argument("target-size requires positive WxH with at most one auto axis");
            }
        }
        else if (!parseSize(value, width, height))
        {
            throw std::invalid_argument(std::string(name) + " requires positive WxH");
        }
        fields[target ? "outW" : std::string(name) == "match-tile" ? "tileW" : "nativeTileW"] = width;
        fields[target ? "outH" : std::string(name) == "match-tile" ? "tileH" : "nativeTileH"] = height;
    }
    if (args.has("brightness-jitter"))
    {
        fields["colorAdjust"] = fields["colorStrength"].get<double>() != 0;
    }
    return fields;
}

OutputMode modeFor(const Arguments &args)
{
    if (args.has("mode"))
    {
        return parseOutputMode(args.get("mode"));
    }
    auto extension = pathToUtf8(u8path(args.get("output")).extension());
    if (!extension.empty())
    {
        extension.erase(0, 1);
    }
    extension = detail::normalizeOutputFormat(extension);
    if (extension == "jpg" || extension == "png" || extension == "webp" || extension == "tiff")
    {
        return OutputMode::Image;
    }
    throw std::invalid_argument("--mode is required when output has no recognized image suffix");
}

void validateEncodingOptions(const Arguments &args, OutputMode mode)
{
    if (mode == OutputMode::Html)
    {
        for (const auto &name : {"format", "quality", "png-compression", "write-mode", "thumbnail"})
        {
            if (args.has(name))
            {
                throw std::invalid_argument("HTML is incompatible with --" + std::string(name));
            }
        }
    }
    if (mode == OutputMode::Tiles || mode == OutputMode::DeepZoom)
    {
        if ((args.has("format") && detail::normalizeOutputFormat(args.get("format")) != "jpg") ||
            args.has("png-compression") || args.has("write-mode") || args.has("thumbnail"))
        {
            throw std::invalid_argument(
                "tiles/deepzoom currently encode JPEG and do not support the selected encoding options");
        }
    }
}

// 核心旧输出仍有回车式进度；命令入口统一为无控制字符的诊断流。
class DiagnosticBuffer : public std::streambuf
{
  public:
    explicit DiagnosticBuffer(bool quiet) : quiet(quiet) {}

  protected:
    int overflow(int value) override
    {
        if (value == traits_type::eof())
        {
            return traits_type::not_eof(value);
        }
        const char c = static_cast<char>(value);
        if (!quiet && (c == '\n' || c == '\t' || static_cast<unsigned char>(c) >= 32))
        {
            std::cerr.rdbuf()->sputc(c);
        }
        return value;
    }
    int sync() override
    {
        std::cerr.rdbuf()->pubsync();
        return 0;
    }

  private:
    bool quiet;
};

ServiceResult execute(const Arguments &args, json &data, RuntimeContext &runtime)
{
    const auto &cmd = args.command;
    if (cmd == "devices")
    {
        data["devices"] = computeDevicesToJson();
        return ServiceResult::success("compute devices enumerated");
    }
    if (args.input.empty())
    {
        throw std::invalid_argument("a positional input is required; use --help");
    }
    if (cmd == "create" || cmd == "render")
    {
        if (!args.has("output"))
        {
            throw std::invalid_argument("--output is required");
        }
        const auto mode = modeFor(args);
        validateEncodingOptions(args, mode);
        auto fields = requestFields(args);
        fields["inputPath"] = cmd == "render" ? "target.png" : args.input;
        fields["outputMode"] = outputModeName(mode);
        MosaicRequest request;
        RequestError error;
        if (!parseMosaicRequestJson(fields.dump(), request, error))
        {
            throw std::invalid_argument(error.message);
        }
        if (cmd == "create")
        {
            return MosaicService().run(request, runtime);
        }
        ProjectRequest project;
        project.projectPath = args.input;
        project.outputPath = request.outputPath;
        project.config = request.config;
        project.overwrite = args.has("overwrite");
        project.outputTileExplicit = args.has("output-tile");
        project.blendExplicit = args.has("target-blend");
        project.jitterExplicit = args.has("brightness-jitter");
        return ProjectService().render(project, runtime);
    }
    if (args.group == "project")
    {
        ProjectRequest request;
        request.projectPath = args.input;
        request.outputPath = args.get("output");
        request.overwrite = args.has("overwrite");
        request.deep = args.has("deep");
        ProjectService service;
        if (cmd == "info")
        {
            data = service.info(args.input);
            return ServiceResult::success("project metadata loaded");
        }
        if (cmd == "check")
        {
            return service.check(request, runtime);
        }
        if (request.outputPath.empty())
        {
            throw std::invalid_argument("--output is required for project pack");
        }
        return service.pack(request, runtime);
    }
    if (cmd == "build" || cmd == "add" || cmd == "normalize")
    {
        BuildRequest request;
        request.inputDir = args.input;
        request.outputDir = args.get("output", "library");
        request.appendMode = cmd == "add";
        request.normalizeOnly = cmd == "normalize";
        request.recursive = args.has("recursive");
        request.resourceMonitor = args.has("resource-monitor");
        request.threads = integer(args.get("threads", "0"));
        request.backend = parseBackend(args.get("backend", "auto"));
        request.device = args.get("device", "auto");
        if (request.appendMode)
        {
            request.dbPath = resolveDbPathForService(args.get("library", "library"));
            request.outputDir = pathToUtf8(u8path(request.dbPath).parent_path());
        }
        else if (args.has("size") && !parseSize(args.get("size"), request.normalizeWidth, request.normalizeHeight))
        {
            throw std::invalid_argument("--size requires positive WxH");
        }
        if (cmd == "build" && std::filesystem::exists(u8path(request.outputDir) / "mosaicraft.db"))
        {
            throw std::invalid_argument("library already exists; use library add or choose a new directory");
        }
        return BuildService().run(request, runtime);
    }
    if (args.group == "library")
    {
        DatabaseService service;
        if (cmd == "report")
        {
            DatabaseRequest request;
            request.dbPath = args.input;
            request.deep = args.has("deep");
            const auto result = service.stats(request);
            data["stats"] = databaseStatsToJson(result.stats);
            if (result.status.ok && (args.has("check") || request.deep))
            {
                const auto health = service.health(request);
                data["health"] = databaseHealthToJson(health.health);
                return health.status;
            }
            return result.status;
        }
        if (cmd == "usage")
        {
            DatabaseUsageRequest request;
            request.dbPath = args.input;
            request.limit = integer(args.get("limit", "50"));
            const auto sort = args.get("sort", "targets");
            if (request.limit < 0 || (sort != "targets" && sort != "tiles"))
            {
                throw std::invalid_argument("usage requires --limit >=0 and --sort targets|tiles");
            }
            request.sortByTiles = sort == "tiles";
            request.showUnused = args.has("unused");
            const auto result = service.usage(request);
            data = databaseUsageToJson(result.usage);
            data["meaning"] = "runs counts distinct recorded targets; unused means absent from recorded history";
            return result.status;
        }
        if (cmd == "export")
        {
            DatabaseUsageExportRequest request;
            request.dbPath = args.input;
            request.outputDir = args.get("output");
            request.selection = args.get("selection", "used");
            request.confirm = true;
            const auto result = service.exportUsage(request);
            data = databaseUsageExportToJson(result.exportInfo);
            return result.status;
        }
        DatabasePurgeRequest request;
        request.dbPath = args.input;
        request.dryRun = !args.has("apply");
        request.confirm = args.has("apply");
        const auto result = service.purge(request);
        data = databasePurgeToJson(result.purge);
        return result.status;
    }
    InspectRequest request;
    request.imagePath = args.input;
    request.dbPath = args.get("library", "library/mosaicraft.db");
    const auto result = InspectService().inspect(request);
    data["imageFeatures"] = inspectResultToJson(result);
    if (result.status.ok && result.databaseAvailable)
    {
        auto fields = requestFields(args);
        fields["inputPath"] = args.input;
        MosaicRequest coverageRequest;
        RequestError error;
        if (!parseMosaicRequestJson(fields.dump(), coverageRequest, error))
        {
            throw std::invalid_argument(error.message);
        }
        MosaicCoverage coverage;
        const auto status = measureMosaicCoverage(coverageRequest, coverage);
        data["gridCoverage"] = {{"columns", coverage.columns},
                                {"rows", coverage.rows},
                                {"minimumCandidates", coverage.minimumCandidates},
                                {"noCandidates", coverage.noCandidates},
                                {"fewCandidates", coverage.fewCandidates},
                                {"threshold", coverage.threshold}};
        return status;
    }
    return result.status;
}

int runMain(const std::vector<std::string> &argv)
{
    bool asJson = std::find(argv.begin(), argv.end(), "--json") != argv.end();
    bool quiet = std::find(argv.begin(), argv.end(), "--quiet") != argv.end();
    ServiceResult result;
    json data;
    Arguments args;
    RuntimeContext runtime(false);
    try
    {
        if (argv.size() == 2 && (argv[1] == "--version" || argv[1] == "-V"))
        {
            std::cout << "Mosaicraft " << kVersion << '\n';
            return 0;
        }
        args = parse(argv);
        if (args.command.empty() || args.has("help") || args.has("help-all"))
        {
            help(args.group, args.command, args.has("help-all"));
            return 0;
        }
        DiagnosticBuffer buffer(quiet);
        struct StreamGuard
        {
            std::streambuf *previous;
            ~StreamGuard()
            {
                std::cout.rdbuf(previous);
            }
        } guard{std::cout.rdbuf(&buffer)};
        ConsoleQuickEditGuard console;
        TaskResourceMonitor resources(runtime, args.has("resource-monitor"));
        result = execute(args, data, runtime);
    }
    catch (const std::bad_alloc &)
    {
        result = ServiceResult::failure(3, "not enough memory", "out_of_memory");
    }
    catch (const std::exception &error)
    {
        result = ServiceResult::failure(1, error.what(), "invalid_argument");
    }
    if (!runtime.snapshot().diagnostic.code.empty() || !runtime.snapshot().artifacts.empty())
    {
        result.runtime = runtime.snapshot();
    }
    if (!result.ok)
    {
        std::cerr << "ERROR: " << result.message << '\n';
    }
    auto output = serviceResultToJson(result);
    output["command"] = args.group.empty() ? args.command : args.group + " " + args.command;
    if (!data.is_null())
    {
        output["data"] = std::move(data);
    }
    if (asJson || quiet)
    {
        std::cout << output.dump() << '\n';
    }
    else
    {
        std::cout << result.message << '\n';
        for (const auto &artifact : result.runtime.artifacts)
        {
            std::cout << artifact.id << ": " << artifact.path << '\n';
        }
        if (output.contains("data"))
        {
            std::cout << output["data"].dump(2) << '\n';
        }
    }
    return result.exitCode;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t *argv[])
#else
int main(int argc, char *argv[])
#endif
{
    std::vector<std::string> arguments;
    for (int i = 0; i < argc; ++i)
    {
#ifdef _WIN32
        arguments.push_back(pathToUtf8(std::filesystem::path(argv[i])));
#else
        arguments.push_back(argv[i]);
#endif
    }
    return runMain(arguments);
}
