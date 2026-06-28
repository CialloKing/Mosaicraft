// Mosaicraft Web UI local HTTP server.
// Serves the Web UI and structured API while keeping legacy command compatibility.
#include "core/httplib.h"
#include "core/ApiHandlers.h"
#include "core/JobManager.h"
#include "core/json.hpp"
#include "core/LegacyRun.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#define popen _popen
#define pclose _pclose
#endif

namespace
{

using json = nlohmann::json;

static bool envFlagEnabled(const char* name)
{
    const char* value = std::getenv(name);
    if (!value) return false;
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text == "1" || text == "true" || text == "yes" || text == "on";
}

static void setJsonBody(httplib::Response& res, const json& body)
{
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

static void sendApiResponse(httplib::Response& res, const mosaicraft::ApiResponse& response)
{
    res.status = response.status;
    setJsonBody(res, response.body);
}

static void handleApi(httplib::Response& res,
                      mosaicraft::JobManager& jobs,
                      mosaicraft::ApiRequest request)
{
    sendApiResponse(res, mosaicraft::handleApiRequest(request, jobs));
}

static mosaicraft::ApiQueryParams queryParams(const httplib::Request& req,
                                              const std::vector<const char*>& keys)
{
    mosaicraft::ApiQueryParams query;
    for (const char* key : keys) {
        if (req.has_param(key)) query[key] = req.get_param_value(key);
    }
    return query;
}

static mosaicraft::ApiQueryParams queryParamsFor(const httplib::Request& req,
                                                 mosaicraft::ApiOperation operation)
{
    return queryParams(req, mosaicraft::apiQueryKeys(operation));
}

static std::string httpPatternForEndpoint(const mosaicraft::ApiEndpointMetadata& endpoint)
{
    if (endpoint.path == "/api/jobs/{id}") {
        return R"(/api/jobs/([A-Za-z0-9_-]+))";
    }
    return endpoint.path;
}

static mosaicraft::ApiRequest apiRequestFromHttp(const httplib::Request& req,
                                                 const mosaicraft::ApiEndpointMetadata& endpoint,
                                                 mosaicraft::ApiRequestContext context = {})
{
    switch (endpoint.requestShape)
    {
    case mosaicraft::ApiRequestShape::None:
        break;
    case mosaicraft::ApiRequestShape::Body:
        context.body = req.body;
        break;
    case mosaicraft::ApiRequestShape::Query:
        context.body = req.body;
        context.query = queryParamsFor(req, endpoint.operation);
        break;
    case mosaicraft::ApiRequestShape::JobId:
        if (context.id.empty() && req.matches.size() > 1) {
            context.id = req.matches[1].str();
        }
        break;
    case mosaicraft::ApiRequestShape::LegacyCommand:
        context.body = req.body;
        break;
    }
    return mosaicraft::apiOperationRequest(endpoint.operation, std::move(context));
}

static mosaicraft::ApiRequestContext discoveryContext(mosaicraft::ApiOperation operation,
                                                      bool legacyRunEnabled)
{
    mosaicraft::ApiRequestContext context;
    if (operation == mosaicraft::ApiOperation::Endpoints ||
        operation == mosaicraft::ApiOperation::Info) {
        context.legacyRunEnabled = legacyRunEnabled;
    }
    if (operation == mosaicraft::ApiOperation::Info) {
        context.entryName = "MosaicraftWebUI";
    }
    return context;
}

static void registerApiMethod(httplib::Server& svr,
                              const std::string& method,
                              mosaicraft::JobManager& jobManager,
                              const mosaicraft::ApiEndpointMetadata& endpoint,
                              bool legacyRunEnabled)
{
    const std::string pattern = httpPatternForEndpoint(endpoint);
    auto handler = [&, endpoint, legacyRunEnabled](const httplib::Request& req, httplib::Response& res) {
        handleApi(res, jobManager,
            apiRequestFromHttp(req, endpoint, discoveryContext(endpoint.operation, legacyRunEnabled)));
    };

    if (method == "GET") {
        svr.Get(pattern, handler);
    } else if (method == "POST") {
        svr.Post(pattern, handler);
    } else if (method == "DELETE") {
        svr.Delete(pattern, handler);
    } else {
        throw std::logic_error("unsupported API endpoint method");
    }
}

static void registerStructuredApiRoutes(
    httplib::Server& svr,
    mosaicraft::JobManager& jobManager,
    const std::vector<mosaicraft::ApiEndpointMetadata>& apiEndpoints,
    bool legacyRunEnabled)
{
    for (const auto& endpoint : apiEndpoints) {
        if (endpoint.operation == mosaicraft::ApiOperation::LegacyRunDisabled) {
            continue;
        }

        if (endpoint.method == "GET|POST") {
            registerApiMethod(svr, "GET", jobManager, endpoint, legacyRunEnabled);
            registerApiMethod(svr, "POST", jobManager, endpoint, legacyRunEnabled);
        } else {
            registerApiMethod(svr, endpoint.method, jobManager, endpoint, legacyRunEnabled);
        }
    }
}

} // namespace

#ifdef _WIN32
static std::string wideToUtf8(const std::wstring& wide)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

static std::wstring utf8ToWide(const std::string& utf8)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), len);
    return out;
}
#endif

static std::string readFile(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return "File not found: " + path;
    return std::string(std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>());
}

static std::string findHtml()
{
    std::vector<std::string> candidates;

#ifdef _WIN32
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
#else
    std::filesystem::path exeDir = std::filesystem::canonical("/proc/self/exe").parent_path();
#endif
    // Published layout: same directory as the executable.
    candidates.push_back((exeDir / "index.html").string());
    candidates.push_back((exeDir / "tools/command-builder/index.html").string());
    // Development layout: build/Release to tools/command-builder.
    candidates.push_back((exeDir / "../../tools/command-builder/index.html").string());
    // Development layout: build to tools/command-builder.
    candidates.push_back((exeDir / "../tools/command-builder/index.html").string());
    // Common paths relative to the current working directory.
    candidates.push_back("index.html");
    candidates.push_back("tools/command-builder/index.html");
    candidates.push_back("../tools/command-builder/index.html");
    candidates.push_back("../../tools/command-builder/index.html");

    for (const auto& c : candidates) {
        try {
            if (std::filesystem::exists(c)) {
                return std::filesystem::canonical(c).string();
            }
        } catch (...) {}
    }
    return "";  // not found
}

static std::string findMosaicraft()
{
#ifdef _WIN32
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path candidate = exeDir / "mosaicraft.exe";
    if (std::filesystem::exists(candidate)) return wideToUtf8(candidate.wstring());
#else
    // Linux: resolve /proc/self/exe to the executable directory.
    std::filesystem::path exePath = std::filesystem::canonical("/proc/self/exe");
    std::filesystem::path exeDir = exePath.parent_path();
    std::filesystem::path candidate = exeDir / "mosaicraft";
    if (std::filesystem::exists(candidate)) return candidate.string();
#endif
    return "mosaicraft";  // hope it's in PATH
}

int main(int argc, char* argv[])
{
    int port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
        if (port < 1 || port > 65535) {
            std::cerr << "ERROR: Invalid port " << argv[1] << " (1-65535)" << std::endl;
            return 1;
        }
    }

    const bool legacyRunEnabled = envFlagEnabled("MOSAICRAFT_ENABLE_LEGACY_RUN");
    std::string mosaicPath = findMosaicraft();
    std::string htmlPath = findHtml();
    if (htmlPath.empty()) {
        std::cerr << "ERROR: Cannot find index.html" << std::endl;
        return 1;
    }

    std::cout << "Mosaicraft Web UI" << std::endl;
    std::cout << "  Server: http://localhost:" << port << std::endl;
    std::cout << "  Mosaicraft: " << mosaicPath << std::endl;
    std::cout << "  HTML: " << htmlPath << std::endl;
    std::cout << "  Legacy /api/run: " << (legacyRunEnabled ? "enabled" : "disabled") << std::endl;

    std::string htmlContent = readFile(htmlPath);
    std::mutex htmlMutex;
    std::mutex runMutex;
    mosaicraft::JobManager jobManager;
    const auto apiEndpoints = mosaicraft::apiEndpointMetadata(legacyRunEnabled);

    httplib::Server svr;

    // Long-running build and mosaic jobs may take minutes.
    svr.set_read_timeout(1800);   // 30 min
    svr.set_write_timeout(1800);

    // Home page.
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(htmlMutex);
        res.set_content(htmlContent, "text/html; charset=utf-8");
    });

    // Reload HTML during local development.
    svr.Get("/reload", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(htmlMutex);
        htmlContent = readFile(htmlPath);
        res.set_content("HTML reloaded", "text/plain");
    });

    registerStructuredApiRoutes(svr, jobManager, apiEndpoints, legacyRunEnabled);

    // Legacy command endpoint.
    svr.Post("/api/run", [&, legacyRunEnabled](const httplib::Request& req, httplib::Response& res) {
        if (!legacyRunEnabled) {
            handleApi(res, jobManager, mosaicraft::apiLegacyRunDisabledRequest());
            return;
        }

        std::unique_lock<std::mutex> runLock(runMutex, std::try_to_lock);
        if (!runLock.owns_lock()) {
            res.status = 429;
            res.set_content("ERROR: another mosaicraft job is already running", "text/plain");
            return;
        }

        auto validation = mosaicraft::validateLegacyRunCommand(req.body);
        if (!validation.ok) {
            res.set_content(validation.error, "text/plain");
            return;
        }
        const std::string& subCmd = validation.subCommand;

        // Use CreateProcess on Windows to preserve Unicode paths.
        std::string output;
        try {
            std::cout << "[RUN] " << mosaicPath << " " << subCmd << std::endl;
#ifdef _WIN32
            HANDLE hReadPipe, hWritePipe;
            SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
                res.set_content("ERROR: CreatePipe failed", "text/plain");
                return;
            }
            SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

            PROCESS_INFORMATION pi = {};
            STARTUPINFOW si = {sizeof(STARTUPINFOW)};
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = hWritePipe;
            si.hStdError = hWritePipe;

            std::wstring wCmd = utf8ToWide(mosaicPath);
            if (wCmd.empty()) {
                CloseHandle(hReadPipe);
                CloseHandle(hWritePipe);
                res.set_content("ERROR: invalid executable path", "text/plain");
                return;
            }
            // subCmd is UTF-8 from the browser request.
            std::wstring wArgs = utf8ToWide(subCmd);
            std::wstring cmdLine = L"\"" + wCmd + L"\" " + wArgs;

            // Keep the command line buffer writable for CreateProcessW.
            std::vector<wchar_t> cmdBuf(cmdLine.size() + 1);
            wcscpy(cmdBuf.data(), cmdLine.c_str());

            if (!CreateProcessW(wCmd.c_str(), cmdBuf.data(),
                               nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(hReadPipe);
                CloseHandle(hWritePipe);
                std::cerr << "[ERROR] CreateProcess failed (" << GetLastError() << ")" << std::endl;
                res.set_content("ERROR: failed to start process", "text/plain");
                return;
            }
            CloseHandle(hWritePipe);
            CloseHandle(pi.hThread);

            char buf[4096];
            DWORD bytesRead;
            while (ReadFile(hReadPipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
                std::cout.write(buf, bytesRead);
                output.append(buf, bytesRead);
            }
            CloseHandle(hReadPipe);

            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD exitCode = 0;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hProcess);

            if (exitCode == 0)
                res.set_content(output.empty() ? "OK" : output, "text/plain; charset=utf-8");
            else
                res.set_content("EXIT " + std::to_string(exitCode) + "\n" + output, "text/plain; charset=utf-8");
#else
            // Linux/macOS fallback for the legacy command endpoint.
            std::string fullCmd = "\"" + mosaicPath + "\" " + subCmd;
            FILE* pipe = popen((fullCmd + " 2>&1").c_str(), "r");
            if (!pipe) {
                std::cerr << "[ERROR] popen failed: " << fullCmd << std::endl;
                res.set_content("ERROR: failed to start process", "text/plain");
                return;
            }
            setvbuf(pipe, nullptr, _IONBF, 0);
            char ch;
            while (fread(&ch, 1, 1, pipe) == 1) {
                std::cout << ch;
                output += ch;
            }
            int rc = pclose(pipe);
            if (rc == 0)
                res.set_content(output.empty() ? "OK" : output, "text/plain; charset=utf-8");
            else
                res.set_content("EXIT " + std::to_string(rc) + "\n" + output, "text/plain; charset=utf-8");
#endif
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Exception in /api/run: " << e.what() << std::endl;
            res.set_content(std::string("ERROR: ") + e.what(), "text/plain");
        } catch (...) {
            std::cerr << "[ERROR] Unknown exception in /api/run" << std::endl;
            res.set_content("ERROR: internal error", "text/plain");
        }
    });

    std::cout << "  Listening on http://localhost:" << port << std::endl;
    std::cout << "  Press Ctrl+C to stop" << std::endl;

    // Try to open the browser.
#ifdef _WIN32
    std::string url = "http://localhost:" + std::to_string(port);
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string url = "http://localhost:" + std::to_string(port);
    std::system(("xdg-open " + url + " &").c_str());
#endif

    if (!svr.listen("localhost", port)) {
        std::cerr << "ERROR: Failed to start server on port " << port << std::endl;
        std::cerr << "  (port may be in use. Try: MosaicraftWebUI " << (port+1) << ")" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return 1;
    }
    // listen() returns only on failure or shutdown.
    std::cout << std::endl << "Server stopped." << std::endl;
    // Brief pause so double-click launches can show the stop message.
#ifdef _WIN32
    std::this_thread::sleep_for(std::chrono::seconds(3));
#endif
    return 0;
}
