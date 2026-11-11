// Mosaicraft Web UI local HTTP server.
// Serves the Web UI and structured API.
#include "core/httplib.h"
#include "core/ApiHandlers.h"
#include "core/ApiJson.h"
#include "core/JobManager.h"
#include "core/json.hpp"
#include "core/ExecutablePath.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#endif
#include <filesystem>
#include <fstream>
#include <memory>
#include <iomanip>
#include "core/UnicodeIO.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{

using json = nlohmann::json;

static void setJsonBody(httplib::Response& res, const json& body)
{
    res.set_content(body.dump(-1, ' ', false, json::error_handler_t::replace), "application/json; charset=utf-8");
}

static bool validUtf8(const std::string& value)
{
    if (value.find('\0') != std::string::npos)
    {
        return false;
    }
    try
    {
        json(value).dump();
        return true;
    }
    catch (const json::exception&)
    {
        return false;
    }
}

static void sendApiResponse(httplib::Response& res, const mosaicraft::ApiResponse& response)
{
    res.status = response.status;
    if (!response.jpeg.empty())
    {
        res.set_header("Cache-Control", "no-store");
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_content(response.jpeg, "image/jpeg");
        return;
    }
    if (response.file)
    {
        const auto &file = *response.file;
        auto stream = std::make_shared<std::ifstream>(mosaicraft::u8path(file.path), std::ios::binary);
        if (!*stream)
        {
            res.status = 404;
            setJsonBody(res, {{"ok", false}, {"message", "artifact cannot be opened"}});
            return;
        }
        std::ostringstream encoded;
        for (unsigned char ch : mosaicraft::pathToUtf8(mosaicraft::u8path(file.path).filename()))
        {
            if (std::isalnum(ch) && ch < 128)
            {
                encoded << ch;
            }
            else
            {
                encoded << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << int(ch);
            }
        }
        res.set_header("Content-Disposition", (file.kind == "preview" ? "inline" : "attachment") +
                                                  std::string("; filename=\"download\"; filename*=UTF-8''") +
                                                  encoded.str());
        res.set_header("Cache-Control", "no-store");
        res.set_header("X-Content-Type-Options", "nosniff");
        // httplib 为 Range 请求选择 206，并据此计算 Content-Length。
        res.status = -1;
        res.set_header("Accept-Ranges", "bytes");
        res.set_content_provider(static_cast<size_t>(file.bytes), file.mime,
                                 [stream](size_t offset, size_t length, httplib::DataSink &sink)
                                 {
                                     // 下载及 Range 响应只使用固定缓冲，不复制整个大文件。
                                     char buffer[64 * 1024];
                                     stream->clear();
                                     stream->seekg(static_cast<std::streamoff>(offset));
                                     while (length > 0)
                                     {
                                         const size_t size = std::min(length, sizeof(buffer));
                                         stream->read(buffer, static_cast<std::streamsize>(size));
                                         if (stream->gcount() != static_cast<std::streamsize>(size) ||
                                             !sink.write(buffer, size))
                                         {
                                             return false;
                                         }
                                         length -= size;
                                     }
                                     return true;
                                 });
        return;
    }
    setJsonBody(res, response.body);
}

static void handleApi(httplib::Response& res,
                      mosaicraft::JobManager& jobs,
                      mosaicraft::ApiRequest request)
{
    if (request.operation == mosaicraft::ApiOperation::GetPreview ||
        request.operation == mosaicraft::ApiOperation::GetPreviewRecommendations ||
        request.operation == mosaicraft::ApiOperation::BrowsePreviewLibrary)
    {
        res.set_header("Cache-Control", "no-store");
    }
    sendApiResponse(res, mosaicraft::handleApiRequest(request, jobs));
}

static mosaicraft::ApiQueryParams queryParams(const httplib::Request& req,
                                              const std::vector<std::string>& keys)
{
    mosaicraft::ApiQueryParams query;
    for (const auto& key : keys) {
        if (req.has_param(key)) query[key] = req.get_param_value(key);
    }
    return query;
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
        context.query = queryParams(req, mosaicraft::apiAcceptedQueryKeyList(endpoint.operation));
        break;
    case mosaicraft::ApiRequestShape::JobQuery:
        context.query = queryParams(req, mosaicraft::apiAcceptedQueryKeyList(endpoint.operation));
        [[fallthrough]];
    case mosaicraft::ApiRequestShape::JobArtifact:
        if (endpoint.requestShape == mosaicraft::ApiRequestShape::JobArtifact && req.matches.size() > 2)
        {
            context.artifactId = req.matches[2].str();
        }
        [[fallthrough]];
    case mosaicraft::ApiRequestShape::JobId:
        if (context.id.empty() && req.matches.size() > 1) {
            context.id = req.matches[1].str();
        }
        break;
    }
    return mosaicraft::apiEndpointRequest(endpoint, std::move(context));
}

#ifndef _WIN32
#endif

static void registerApiMethod(httplib::Server& svr,
                              const std::string& method,
                              mosaicraft::JobManager& jobManager,
                              const mosaicraft::ApiEndpointMetadata& endpoint)
{
    if (endpoint.httpPattern.empty()) {
        throw std::logic_error("API endpoint has no HTTP pattern");
    }
    const std::string pattern = endpoint.httpPattern;
    auto handler = [&, endpoint](const httplib::Request& req, httplib::Response& res) {
        for (const auto &parameter : req.params)
        {
            if (req.get_param_value_count(parameter.first) != 1 ||
                std::find(endpoint.acceptedQueryKeys.begin(), endpoint.acceptedQueryKeys.end(), parameter.first) == endpoint.acceptedQueryKeys.end())
            {
                res.status = 400;
                setJsonBody(res, mosaicraft::apiErrorJson("unknown or repeated query parameter: " + parameter.first));
                return;
            }
        }
        mosaicraft::ApiRequestContext context;
        context.entryName = "MosaicraftWebUI";
        handleApi(res, jobManager,
            apiRequestFromHttp(req, endpoint, std::move(context)));
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
    const std::vector<mosaicraft::ApiEndpointMetadata>& apiEndpoints)
{
    for (const auto& endpoint : apiEndpoints) {

        if (endpoint.methods.empty()) {
            throw std::logic_error("API endpoint has no methods");
        }
        for (const auto& method : endpoint.methods) {
            registerApiMethod(svr, method, jobManager, endpoint);
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
    std::ifstream ifs(std::filesystem::u8path(path), std::ios::binary);
    if (!ifs) return "File not found: " + path;
    return std::string(std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>());
}

static std::string findHtml()
{
    const auto exeDir = mosaicraft::executableDirectory();
    // Keep native paths until returning UTF-8; ANSI conversion can throw on
    // Chinese archive directories even when the published index.html exists.
    const std::vector<std::filesystem::path> candidates
    {
        exeDir / "index.html",
        exeDir / "tools/command-builder/index.html",
        exeDir / "../../tools/command-builder/index.html",
        exeDir / "../tools/command-builder/index.html",
        "index.html", "tools/command-builder/index.html",
        "../tools/command-builder/index.html", "../../tools/command-builder/index.html"
    };
    for (const auto& candidate : candidates)
    {
        try
        {
            if (std::filesystem::exists(candidate))
            {
                return std::filesystem::canonical(candidate).u8string();
            }
        }
        catch (const std::filesystem::filesystem_error&)
        {
            // A missing development layout must not hide another valid layout.
        }
    }
    return "";  // not found
}

static std::string findMosaicraft()
{
    const auto exeDir = mosaicraft::executableDirectory();
#ifdef _WIN32
    std::filesystem::path candidate = exeDir / "mosaicraft.exe";
#else
    std::filesystem::path candidate = exeDir / "mosaicraft";
#endif
    if (std::filesystem::exists(candidate))
    {
        return candidate.u8string();
    }
    return "mosaicraft";  // hope it's in PATH
}

static bool envFlagEnabled(const char* name)
{
    const char* value = std::getenv(name);
    if (!value) return false;
    std::string text(value);
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text == "1" || text == "true" || text == "yes" || text == "on";
}

int main(int argc, char* argv[])
{
    int port = 8080;
    bool openBrowser = !envFlagEnabled("MOSAICRAFT_NO_BROWSER");
    bool portSet = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--no-open" || arg == "--no-browser") {
            openBrowser = false;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: MosaicraftWebUI [port] [--no-open]" << std::endl;
            return 0;
        }
        if (portSet) {
            std::cerr << "ERROR: Unexpected argument " << arg << std::endl;
            std::cerr << "Usage: MosaicraftWebUI [port] [--no-open]" << std::endl;
            return 1;
        }
        errno = 0;
        char* end = nullptr;
        long parsedPort = std::strtol(arg.c_str(), &end, 10);
        if (errno == ERANGE || end == arg.c_str() || *end != '\0' ||
            parsedPort < 1 || parsedPort > 65535) {
            std::cerr << "ERROR: Invalid port " << arg << " (1-65535)" << std::endl;
            return 1;
        }
        port = static_cast<int>(parsedPort);
        portSet = true;
    }

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
    std::cout << "  Browser: " << (openBrowser ? "auto-open" : "disabled") << std::endl;

    std::string htmlContent = readFile(htmlPath);
    std::mutex htmlMutex;
    std::mutex runMutex;
    mosaicraft::JobManager jobManager;
    const auto apiEndpoints = mosaicraft::apiEndpointMetadata();
    const auto endpointErrors = mosaicraft::validateApiEndpointMetadata(apiEndpoints);
    if (!endpointErrors.empty()) {
        std::cerr << "ERROR: Invalid API endpoint metadata" << std::endl;
        for (const auto& error : endpointErrors) {
            std::cerr << "  - " << error << std::endl;
        }
        return 1;
    }

    httplib::Server svr;
    svr.set_error_handler([](const httplib::Request &request, httplib::Response &response)
    {
        if (request.path.rfind("/api/", 0) == 0 && response.get_header_value("Content-Type").find("application/json") == std::string::npos)
        {
            setJsonBody(response, mosaicraft::apiErrorJson("HTTP request failed: " + std::to_string(response.status)));
        }
    });
    svr.set_pre_routing_handler(
        [port](const httplib::Request& request, httplib::Response& response)
        {
            using Result = httplib::Server::HandlerResponse;
            if (request.path.rfind("/api/", 0) != 0)
            {
                return Result::Unhandled;
            }
            auto reject = [&](int status, const char* message)
            {
                response.status = status;
                setJsonBody(response, mosaicraft::apiErrorJson(message));
                return Result::Handled;
            };
            auto host = request.get_header_value("Host");
            const auto suffix = ":" + std::to_string(port);
            if (request.get_header_value_count("Host") != 1 ||
                (host != "localhost" + suffix && host != "127.0.0.1" + suffix && host != "[::1]" + suffix))
            {
                return reject(403, "Host must identify this local server");
            }
            // 回环监听不等于同源授权；浏览器的外部页面和其他端口也不能执行维护操作。
            if ((request.has_header("Origin") && request.get_header_value("Origin") != "http://" + host) ||
                request.get_header_value("Sec-Fetch-Site") == "cross-site")
            {
                return reject(403, "cross-origin API requests are not allowed");
            }
            // 预路由阶段尚未读取 body，必须根据传输头判断，不能检查空的 request.body。
            if (request.has_header("Transfer-Encoding") ||
                (request.has_header("Content-Length") && request.get_header_value("Content-Length") != "0"))
            {
                auto type = request.get_header_value("Content-Type");
                auto separator = type.find(';');
                type = type.substr(0, separator);
                std::transform(type.begin(), type.end(), type.begin(),
                               [](unsigned char c)
                               {
                                   return static_cast<char>(std::tolower(c));
                               });
                // 兼容端点仍接收原始命令文本，同样先通过 Host/Origin 检查。
                if (type != "application/json")
                {
                    return reject(415, "API request body must use application/json");
                }
            }
            for (const auto& parameter : request.params)
            {
                if (!validUtf8(parameter.first) || !validUtf8(parameter.second))
                {
                    return reject(400, "query parameters must contain valid UTF-8 without NUL bytes");
                }
            }
            return Result::Unhandled;
        });
    svr.set_exception_handler(
        [](const httplib::Request&, httplib::Response& response, std::exception_ptr)
        {
            response.status = 500;
            setJsonBody(response, {{"ok", false}, {"message", "internal server error"}});
        });

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

    registerStructuredApiRoutes(svr, jobManager, apiEndpoints);

    std::cout << "  Listening on http://localhost:" << port << std::endl;
    std::cout << "  Press Ctrl+C to stop" << std::endl;

    // Try to open the browser.
    if (openBrowser) {
#ifdef _WIN32
        std::string url = "http://localhost:" + std::to_string(port);
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        std::string url = "http://localhost:" + std::to_string(port);
        pid_t opener = fork();
        if (opener == 0)
        {
#ifdef __APPLE__
            execlp("open", "open", url.c_str(), static_cast<char*>(nullptr));
#else
            execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
#endif
            _exit(127);
        }
#endif
    }

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
