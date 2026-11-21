#include "httplib.h"
#include "json.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace
{
void require(bool condition, const std::string &message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::string read(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Cannot read " + path.u8string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

#ifdef _WIN32
std::wstring quote(const std::wstring &value)
{
    // CreateProcess accepts a command line, not argv. Match the MS CRT's escaping.
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (const wchar_t ch : value)
    {
        if (ch == L'\\')
        {
            ++slashes;
            continue;
        }
        out.append(ch == L'"' ? 2 * slashes + 1 : slashes, L'\\');
        out += ch;
        slashes = 0;
    }
    out.append(2 * slashes, L'\\');
    return out + L'"';
}
#endif

class ServerProcess
{
public:
    ServerProcess(const fs::path &exe, const fs::path &cwd, const fs::path &log, int port)
    {
#ifdef _WIN32
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(output != INVALID_HANDLE_VALUE, "Cannot open server log");
        HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &attributes,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input;
        startup.hStdOutput = startup.hStdError = output;
        PROCESS_INFORMATION info{};
        auto command = quote(exe.native()) + L" " + std::to_wstring(port) + L" --no-open";
        const auto ok = input != INVALID_HANDLE_VALUE &&
                        CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW, nullptr, cwd.c_str(), &startup, &info);
        if (input != INVALID_HANDLE_VALUE)
        {
            CloseHandle(input);
        }
        CloseHandle(output);
        require(ok, "Cannot start Web UI process");
        CloseHandle(info.hThread);
        process_ = info.hProcess;
#else
        const auto number = std::to_string(port);
        pid_ = fork();
        require(pid_ >= 0, "Cannot fork Web UI process");
        if (pid_ == 0)
        {
            const int output = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (output < 0 || dup2(output, STDOUT_FILENO) < 0 || dup2(output, STDERR_FILENO) < 0 ||
                chdir(cwd.c_str()) != 0)
            {
                _exit(126);
            }
            close(output);
            execl(exe.c_str(), exe.c_str(), number.c_str(), "--no-open", static_cast<char *>(nullptr));
            _exit(127);
        }
#endif
    }

    ~ServerProcess()
    {
#ifdef _WIN32
        if (running())
        {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 5000);
        }
        CloseHandle(process_);
#else
        if (running())
        {
            kill(pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR)
            {
            }
        }
#endif
    }
    ServerProcess(const ServerProcess &) = delete;
    ServerProcess &operator=(const ServerProcess &) = delete;

    bool running()
    {
#ifdef _WIN32
        return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
#else
        if (pid_ > 0)
        {
            pid_t status;
            do
            {
                status = waitpid(pid_, nullptr, WNOHANG);
            } while (status < 0 && errno == EINTR);
            if (status == 0)
            {
                return true;
            }
        }
        pid_ = -1;
        return false;
#endif
    }

private:
#ifdef _WIN32
    HANDLE process_ = nullptr;
#else
    pid_t pid_ = -1;
#endif
};

void bitmap(const fs::path &path, int width, int height, int seed)
{
    const int stride = (width * 3 + 3) & ~3;
    std::ofstream out(path, std::ios::binary);
    const auto little = [&](unsigned int value, int bytes)
    {
        for (int i = 0; i < bytes; ++i)
        {
            out.put(static_cast<char>((value >> (8 * i)) & 255));
        }
    };
    out.write("BM", 2);
    little(54 + stride * height, 4);
    little(0, 4);
    little(54, 4);
    little(40, 4);
    little(width, 4);
    little(height, 4);
    little(1, 2);
    little(24, 2);
    little(0, 4);
    little(stride * height, 4);
    for (int i = 0; i < 4; ++i)
    {
        little(0, 4);
    }
    for (int y = height - 1; y >= 0; --y)
    {
        for (int x = 0; x < width; ++x)
        {
            little(x + 71 * seed, 1);
            little(y + 43 * seed, 1);
            little(x + y + 29 * seed, 1);
        }
        little(0, stride - width * 3);
    }
    require(static_cast<bool>(out), "Could not write BMP fixture");
}

class Api
{
public:
    explicit Api(int port) : client_("localhost", port)
    {
        client_.set_connection_timeout(1, 0);
        client_.set_read_timeout(20, 0);
    }

    void wait(ServerProcess &process)
    {
        const auto deadline = Clock::now() + 15s;
        do
        {
            require(process.running(), "Web UI exited before accepting requests");
            if (const auto result = client_.Get("/api/info"); result && result->status == 200)
            {
                return;
            }
            std::this_thread::sleep_for(100ms);
        } while (Clock::now() < deadline);
        throw std::runtime_error("Web UI readiness timeout");
    }

    httplib::Response request(const std::string &method, const std::string &path,
                              const std::string &body = {}, const httplib::Headers &headers = {})
    {
        httplib::Request request;
        request.method = method;
        request.path = path;
        request.body = body;
        request.headers = headers;
        if (method == "POST")
        {
            request.set_header("Content-Type", "application/json");
        }
        auto result = client_.send(request);
        require(static_cast<bool>(result), method + " " + path + ": " + httplib::to_string(result.error()));
        return *result;
    }

    Json json(const std::string &path, const Json &body = nullptr)
    {
        const auto result = request(body.is_null() ? "GET" : "POST", path, body.is_null() ? "" : body.dump());
        require(result.status >= 200 && result.status < 300, path + ": " + result.body);
        const auto parsed = Json::parse(result.body);
        require(parsed.value("ok", false), path + ": " + result.body);
        return parsed;
    }

    Json job(const std::string &operation, const Json &body, const std::string &input,
             const std::string &output)
    {
        const auto initial = json("/api/jobs/" + operation, body).at("job");
        const auto id = initial.at("id").get<std::string>();
        const auto snapshot = [&](const Json &job)
        {
            require(!id.empty() && job.at("id") == id && job.at("type") == operation &&
                    job.at("inputPath") == input && (output.empty() || job.at("outputPath") == output) &&
                    job.at("createdAt").get<int64_t>() > 0 && job.at("ok").is_boolean() &&
                    job.at("exitCode").is_number_integer(), "Invalid " + operation + " job snapshot");
        };
        snapshot(initial);
        const auto deadline = Clock::now() + 45s;
        do
        {
            const auto current = json("/api/jobs/" + id).at("job");
            const auto state = current.at("state");
            if (state == "succeeded")
            {
                snapshot(current);
                require(current.at("ok") == true && current.at("exitCode") == 0 &&
                        current.at("startedAt").get<int64_t>() > 0 && current.at("finishedAt") >= current.at("startedAt"),
                        "Invalid completion state: " + current.dump());
                return current;
            }
            require(state != "failed" && state != "canceled", operation + ": " + current.dump());
            std::this_thread::sleep_for(100ms);
        } while (Clock::now() < deadline);
        throw std::runtime_error(operation + " job timed out: " + id);
    }

private:
    httplib::Client client_;
};

void scenarios(Api &api, const fs::path &root, const Json &contracts, bool vulkanFallback)
{
    const auto input = root / "input";
    fs::create_directory(input);
    const auto target = root / "target.bmp";
    for (int i = 0; i < 10; ++i)
    {
        bitmap(input / ("tile-" + std::to_string(i) + ".bmp"), 72, 128, i);
    }
    bitmap(target, 36, 64, 11);
    const auto library = (root / "library").u8string();
    const auto database = (root / "library" / "mosaicraft.db").u8string();
    fs::create_directory(root / "library");
    const auto output = [&](const std::string &name) { return (root / name).u8string(); };
    const auto page = api.request("GET", "/");
    require(page.status == 200 && page.body.find("<!DOCTYPE html>") != std::string::npos,
            "Missing installed Web UI HTML document");
    const auto info = api.json("/api/info").at("info");
    require(info.at("api").at("structured") == true &&
            info.at("api").at("contractMajorVersion") == contracts.at("apiMajorVersion") &&
            info.at("api").at("contractVersion") == contracts.at("apiVersion"), "Unexpected API contract");
    const auto endpoints = api.json("/api/endpoints").at("endpoints");
    for (const auto operation : {"info", "endpoints", "submitMosaicJob", "submitBuildJob", "getJob", "cancelJob",
         "clearFinishedJobs", "databaseStats", "databaseHealth", "databaseUsage", "databaseUsageExport",
         "databasePurge", "inspect", "planMosaic", "mosaicCoverage", "openCatalog", "submitMosaicSample",
         "previewQuality", "pinComparison", "submitComparison", "listHistory", "clearHistory"})
    {
        require(std::any_of(endpoints.begin(), endpoints.end(), [&](const Json &entry)
        {
            return entry.at("operation") == operation;
        }), std::string("Missing operation: ") + operation);
    }
    require(api.request("POST", "/api/jobs/mosaic", "{").status == 400, "Malformed JSON must be rejected");
    require(api.request("GET", "/api/jobs/not-found").status == 404, "Unknown job must be rejected");
    require(api.request("POST", "/api/run", R"({"command":"mosaicraft db-stats"})").status == 404,
            "Legacy command endpoint must not exist");
    for (const auto query : {"db=x", "dbPath=x&unknown=1", "dbPath=x&dbPath=y", "dbPath=x&showUnused=1"})
    {
        require(api.request("GET", std::string("/api/db/usage?") + query).status == 400, "Invalid usage query accepted");
    }

    Json build{{"inputDir", input.u8string()}, {"outputDir", library}, {"dbPath", database}, {"threads", 1},
               {"forceMode", true}, {"normalizeWidth", 36}, {"normalizeHeight", 64}, {"jpgQuality", 82}, {"pngLevel", 0}};
    const auto built = api.job("build", build, input.u8string(), library);
    const auto fallback = [](const Json &runtime)
    {
        const auto reason = runtime.at("fallbackReason").get<std::string>();
        require(runtime.at("featureBackend") == "cpu" && (reason.find("Vulkan") != std::string::npos ||
                reason.find("vk") != std::string::npos), "Missing Vulkan fallback to CPU");
    };
    if (vulkanFallback)
    {
        fallback(built.at("runtime"));
    }
    require(fs::file_size(fs::u8path(database)) > 0, "Build did not create the database");
    api.json("/api/db/stats", {{"dbPath", database}});
    const auto inspect = api.json("/api/inspect", {{"imagePath", target.u8string()}}).at("inspect");
    require(inspect.at("width") == 36 && inspect.at("height") == 64, "Target dimensions were not preserved");

    Json mosaic{{"inputPath", target.u8string()}, {"dbPath", database}, {"outputPath", output("mosaic.png")},
                {"tileW", 9}, {"tileH", 16}, {"outW", 18}, {"outH", 32}, {"nativeTileW", 18},
                {"nativeTileH", 32}, {"candidates", 10}, {"topNRandom", 1}, {"pngLevel", 1},
                {"format", "png"}, {"threads", 2}, {"useGpu", false}};
    const auto plan = api.json("/api/mosaic/plan", mosaic).at("plan");
    require(plan.at("columns") == 2 && plan.at("rows") == 2 && plan.at("rawBytes") == 36 * 64 * 3 &&
            !fs::exists(root / "mosaic.png"), "Mosaic planning must report the grid without creating an image");
    auto scale = mosaic;
    scale.erase("outW");
    scale.erase("outH");
    scale["targetSize"] = "50%";
    const auto scaled = api.json("/api/mosaic/plan", scale).at("plan");
    require(scaled.at("columns") == plan.at("columns") && scaled.at("rows") == plan.at("rows"),
            "Percent target size did not produce the expected grid");
    const auto mosaicJob = api.job("mosaic", scale, target.u8string(), output("mosaic.png"));
    const auto id = mosaicJob.at("id").get<std::string>();
    const auto artifact = "/api/jobs/" + id + "/artifacts/main";
    const auto image = api.request("GET", artifact);
    require(image.status == 200 && image.body == read(root / "mosaic.png"), "Artifact differs from saved image");
    const auto partial = api.request("GET", artifact, {}, {{"Range", "bytes=0-7"}});
    require(partial.status == 206 && partial.body == image.body.substr(0, 8), "Invalid artifact range response");
    require(api.json("/api/jobs/" + id + "/events?after=0&limit=100").contains("nextCursor"), "Missing event cursor");

    auto html = mosaic;
    html["outputMode"] = "html";
    html["outputPath"] = output("portable-artwork");
    for (const auto field : {"format", "pngLevel"})
    {
        html.erase(field);
    }
    const auto project = api.job("mosaic", html, target.u8string(), output("portable-artwork"));
    const auto projectId = project.at("id").get<std::string>();
    require(api.request("GET", "/api/jobs/" + projectId + "/artifacts/main").status == 409,
            "HTML export must require the complete project archive");
    const auto manifest = api.json("/api/project/info", {{"projectPath", output("portable-artwork")}}).at("project");
    require(manifest.at("assetCount").get<int>() > 0, "Missing portable project assets");
    api.job("project-check", {{"projectPath", output("portable-artwork")}, {"deep", true}}, output("portable-artwork"), "");
    const auto rendered = api.job("render", {{"projectPath", output("portable-artwork")},
                                  {"outputPath", output("project-render.png")}, {"threads", 2}, {"benchmark", true}},
                                  output("portable-artwork"), output("project-render.png")).at("runtime");
    require(rendered.at("featureBackend") == "none" && rendered.at("scoreBackend") == "none" &&
            rendered.at("metrics").at("threadsRequested") == 2, "Portable render ran matching or lost thread settings");
    const auto packed = api.job("project-pack", {{"projectPath", output("portable-artwork")},
                                 {"outputPath", output("portable-artwork.zip")}}, output("portable-artwork"),
                                 output("portable-artwork.zip")).at("id").get<std::string>();
    require(api.request("HEAD", "/api/jobs/" + packed + "/artifacts/archive").status == 200,
            "Portable project archive is not downloadable");

    if (vulkanFallback)
    {
        mosaic.erase("useGpu");
        mosaic["outputPath"] = output("automatic.png");
        const auto automatic = api.job("mosaic", mosaic, target.u8string(), output("automatic.png")).at("runtime");
        fallback(automatic);
        require(automatic.at("scoreBackend") == "cpu", "Vulkan fallback scoring was not CPU");
        mosaic["backend"] = "vulkan";
        mosaic["outputPath"] = output("required-vulkan.png");
        const auto failure = Json::parse(api.request("POST", "/api/mosaic", mosaic.dump()).body);
        require(failure.at("ok") == false && failure.at("diagnostic").at("code") == "gpu_required",
                "An explicit Vulkan request must not silently fall back");
    }
    const auto history = api.json("/api/history").at("history");
    require(std::any_of(history.begin(), history.end(), [&](const Json &entry)
    {
        return entry.at("id") == built.at("id") && entry.at("request").at("jpgQuality") == 82 &&
               entry.at("request").at("pngLevel") == 0 && !entry.at("request").contains("webpQuality");
    }), "Build history did not preserve explicit codec options");
    require(api.request("DELETE", "/api/history").status == 200 && fs::exists(root / "mosaic.png"),
            "Clearing history must preserve saved output");
}

int smoke(const fs::path &exe, const fs::path &contractsFile, bool vulkanFallback)
{
    const auto contracts = Json::parse(read(contractsFile));
    std::random_device random;
    const auto root = fs::temp_directory_path() /
                      fs::u8path(u8"mosaicraft \u4e2d\u6587 smoke " + std::to_string(random()) + "-" + std::to_string(random()));
    require(fs::create_directory(root), "Cannot create isolated smoke workspace");
    const auto log = root / "server.log";
    try
    {
        const int port = 19000 + static_cast<int>(random() % 40000);
        ServerProcess process(fs::absolute(exe), root, log, port);
        Api api(port);
        api.wait(process);
        scenarios(api, root, contracts, vulkanFallback);
    }
    catch (...)
    {
        std::error_code error;
        if (fs::exists(log, error))
        {
            std::cerr << read(log) << '\n';
        }
        std::cerr << "Smoke workspace: " << root.u8string() << '\n';
        throw;
    }
    fs::remove_all(root);
    std::cout << "Web UI/API smoke passed\n";
    return 0;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
#else
int main(int argc, char **argv)
#endif
{
    try
    {
        require(argc == 3 || argc == 4,
                "Usage: mosaicraft_release_check <WebUI> <contracts.json> [--vulkan-fallback]");
        require(argc == 3 || fs::path(argv[3]) == "--vulkan-fallback", "Unknown smoke argument");
        return smoke(fs::path(argv[1]), fs::path(argv[2]), argc == 4);
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
