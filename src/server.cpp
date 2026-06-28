// Mosaicraft Web UI — 本地 HTTP 服务器
// 提供命令生成页面 + 结构化 API，兼容旧的 mosaicraft.exe 命令入口
#include "core/httplib.h"
#include "core/json.hpp"
#include "core/MosaicService.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#define popen _popen
#define pclose _pclose
#endif

namespace
{

using json = nlohmann::json;

static void setJsonResult(httplib::Response& res, const mosaicraft::ServiceResult& result)
{
    json body = {
        {"ok", result.ok},
        {"exitCode", result.exitCode},
        {"message", result.message}
    };
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

static bool parseSize(const std::string& text, int& w, int& h)
{
    size_t sep = text.find('x');
    if (sep == std::string::npos) sep = text.find('X');
    if (sep == std::string::npos || sep == 0 || sep + 1 >= text.size()) return false;
    w = std::max(1, std::atoi(text.substr(0, sep).c_str()));
    h = std::max(1, std::atoi(text.substr(sep + 1).c_str()));
    return true;
}

static bool getStringField(const json& body,
                           std::initializer_list<const char*> keys,
                           std::string& out,
                           std::string& error)
{
    if (!error.empty()) return false;
    for (const char* key : keys) {
        auto it = body.find(key);
        if (it == body.end() || it->is_null()) continue;
        if (!it->is_string()) {
            error = std::string(key) + " must be a string";
            return false;
        }
        out = it->get<std::string>();
        return true;
    }
    return false;
}

static bool getIntField(const json& body,
                        std::initializer_list<const char*> keys,
                        int& out,
                        std::string& error)
{
    if (!error.empty()) return false;
    for (const char* key : keys) {
        auto it = body.find(key);
        if (it == body.end() || it->is_null()) continue;
        if (!it->is_number()) {
            error = std::string(key) + " must be a number";
            return false;
        }
        out = static_cast<int>(it->get<double>());
        return true;
    }
    return false;
}

static bool getDoubleField(const json& body,
                           std::initializer_list<const char*> keys,
                           double& out,
                           std::string& error)
{
    if (!error.empty()) return false;
    for (const char* key : keys) {
        auto it = body.find(key);
        if (it == body.end() || it->is_null()) continue;
        if (!it->is_number()) {
            error = std::string(key) + " must be a number";
            return false;
        }
        out = it->get<double>();
        return true;
    }
    return false;
}

static bool getBoolField(const json& body,
                         std::initializer_list<const char*> keys,
                         bool& out,
                         std::string& error)
{
    if (!error.empty()) return false;
    for (const char* key : keys) {
        auto it = body.find(key);
        if (it == body.end() || it->is_null()) continue;
        if (!it->is_boolean()) {
            error = std::string(key) + " must be a boolean";
            return false;
        }
        out = it->get<bool>();
        return true;
    }
    return false;
}

static bool buildMosaicRequest(const std::string& body,
                               mosaicraft::MosaicRequest& request,
                               std::string& error)
{
    json values;
    try {
        values = json::parse(body);
    } catch (const json::parse_error& e) {
        error = std::string("invalid JSON body: ") + e.what();
        return false;
    }
    if (!values.is_object()) {
        error = "JSON body must be an object";
        return false;
    }

    std::string text;
    int intValue = 0;
    double doubleValue = 0.0;
    bool boolValue = false;

    if (getStringField(values, {"inputPath", "input"}, text, error))
        request.inputPath = text;
    if (getStringField(values, {"dbPath", "db"}, text, error))
        request.dbPath = text;
    if (getStringField(values, {"outputPath", "output"}, text, error))
        request.outputPath = text;

    auto& cfg = request.config;
    if (getIntField(values, {"tileW"}, intValue, error)) cfg.tileW = std::max(4, intValue);
    if (getIntField(values, {"tileH"}, intValue, error)) cfg.tileH = std::max(4, intValue);
    if (getIntField(values, {"outW"}, intValue, error)) cfg.outW = intValue > 0 ? intValue : 0;
    if (getIntField(values, {"outH"}, intValue, error)) cfg.outH = intValue > 0 ? intValue : 0;
    if (getIntField(values, {"nativeTileW"}, intValue, error)) cfg.nativeTileW = std::max(1, intValue);
    if (getIntField(values, {"nativeTileH"}, intValue, error)) cfg.nativeTileH = std::max(1, intValue);
    if (getIntField(values, {"candidates"}, intValue, error)) cfg.candidates = std::max(10, intValue);
    if (getIntField(values, {"topNrandom", "topNRandom"}, intValue, error))
        cfg.topNrandom = std::max(1, intValue);
    if (getIntField(values, {"neighborWindow"}, intValue, error)) cfg.neighborWindow = intValue;
    if (getIntField(values, {"upscale"}, intValue, error)) cfg.upscale = std::max(1, intValue);
    if (getIntField(values, {"quality", "jpegQuality"}, intValue, error))
        cfg.jpegQuality = std::max(1, std::min(100, intValue));
    if (getIntField(values, {"pngLevel", "pngCompressionLevel"}, intValue, error))
        cfg.pngCompressionLevel = std::max(1, std::min(9, intValue));

    if (getDoubleField(values, {"lRange"}, doubleValue, error)) cfg.lRange = doubleValue;
    if (getDoubleField(values, {"usePenalty", "penalty"}, doubleValue, error))
        cfg.usePenalty = doubleValue;
    if (getDoubleField(values, {"labWeight"}, doubleValue, error)) cfg.labWeight = doubleValue;
    if (getDoubleField(values, {"gridWeight"}, doubleValue, error)) cfg.gridWeight = doubleValue;
    if (getDoubleField(values, {"tinyWeight"}, doubleValue, error)) cfg.tinyWeight = doubleValue;
    if (getDoubleField(values, {"edgeWeight"}, doubleValue, error)) cfg.edgeWeight = doubleValue;
    if (getDoubleField(values, {"lbpWeight"}, doubleValue, error)) cfg.lbpWeight = doubleValue;
    if (getDoubleField(values, {"neighborPenalty"}, doubleValue, error)) cfg.neighborPenalty = doubleValue;
    if (getDoubleField(values, {"colorStrength"}, doubleValue, error))
        cfg.colorStrength = std::max(0.0, std::min(0.5, doubleValue));

    if (getStringField(values, {"format", "outputFormat"}, text, error)) {
        if (!text.empty()) {
            cfg.outputFormat = text;
            cfg.formatExplicit = true;
        }
    }
    if (getStringField(values, {"writeMode"}, text, error)) {
        if (text == "auto" || text == "stream" || text == "batch") {
            cfg.writeMode = text;
        } else {
            error = "writeMode must be auto, stream, or batch";
            return false;
        }
    }
    if (getStringField(values, {"outputTile"}, text, error)) {
        if (!parseSize(text, cfg.nativeTileW, cfg.nativeTileH)) {
            error = "outputTile must use WxH format";
            return false;
        }
    }

    if (getBoolField(values, {"useGpu"}, boolValue, error)) cfg.useGpu = boolValue;
    if (getBoolField(values, {"cpu"}, boolValue, error) && boolValue) cfg.useGpu = false;
    if (getBoolField(values, {"tiled", "tiledOutput"}, boolValue, error))
        cfg.tiledOutput = boolValue;
    if (getBoolField(values, {"deepZoom", "deepzoom"}, boolValue, error)) {
        cfg.deepZoom = boolValue;
        if (boolValue) cfg.tiledOutput = true;
    }
    if (getBoolField(values, {"colorAdjust"}, boolValue, error)) cfg.colorAdjust = boolValue;
    if (getBoolField(values, {"adaptiveWeights"}, boolValue, error)) cfg.adaptiveWeights = boolValue;
    if (getBoolField(values, {"analyze"}, boolValue, error)) cfg.analyze = boolValue;
    if (getBoolField(values, {"benchmark"}, boolValue, error)) cfg.benchmark = boolValue;

    if (!error.empty()) return false;
    return true;
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
    // exe 同目录（发布版）
    candidates.push_back((exeDir / "index.html").string());
    candidates.push_back((exeDir / "tools/command-builder/index.html").string());
    // 开发目录：build/Release → ../../tools/command-builder/
    candidates.push_back((exeDir / "../../tools/command-builder/index.html").string());
    // 开发目录：build → ../tools/command-builder/
    candidates.push_back((exeDir / "../tools/command-builder/index.html").string());
    // 当前工作目录下的常见路径
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
    // Linux: /proc/self/exe → exe directory
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

    std::string htmlContent = readFile(htmlPath);
    std::mutex htmlMutex;
    std::mutex runMutex;

    httplib::Server svr;

    // 长任务超时设置（建库可能需要数分钟）
    svr.set_read_timeout(1800);   // 30 min
    svr.set_write_timeout(1800);

    // 主页
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(htmlMutex);
        res.set_content(htmlContent, "text/html; charset=utf-8");
    });

    // 重启时重新加载 HTML（开发用）
    svr.Get("/reload", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(htmlMutex);
        htmlContent = readFile(htmlPath);
        res.set_content("HTML reloaded", "text/plain");
    });

    svr.Post("/api/mosaic", [&](const httplib::Request& req, httplib::Response& res) {
        std::unique_lock<std::mutex> runLock(runMutex, std::try_to_lock);
        if (!runLock.owns_lock()) {
            res.status = 429;
            setJsonResult(res, mosaicraft::ServiceResult::failure(
                1, "another mosaicraft job is already running"));
            return;
        }

        mosaicraft::MosaicRequest request;
        std::string error;
        if (!buildMosaicRequest(req.body, request, error)) {
            res.status = 400;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, error));
            return;
        }

        try {
            std::cout << "[API] mosaic input=" << request.inputPath
                      << " db=" << request.dbPath
                      << " output=" << request.outputPath << std::endl;
            mosaicraft::MosaicService service;
            mosaicraft::ServiceResult result = service.run(request);
            if (!result.ok) res.status = 500;
            setJsonResult(res, result);
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Exception in /api/mosaic: " << e.what() << std::endl;
            res.status = 500;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, e.what()));
        } catch (...) {
            std::cerr << "[ERROR] Unknown exception in /api/mosaic" << std::endl;
            res.status = 500;
            setJsonResult(res, mosaicraft::ServiceResult::failure(1, "internal error"));
        }
    });

    // 执行命令
    svr.Post("/api/run", [&](const httplib::Request& req, httplib::Response& res) {
        std::unique_lock<std::mutex> runLock(runMutex, std::try_to_lock);
        if (!runLock.owns_lock()) {
            res.status = 429;
            res.set_content("ERROR: another mosaicraft job is already running", "text/plain");
            return;
        }

        std::string cmd = req.body;
        if (cmd.empty()) {
            res.set_content("ERROR: empty command", "text/plain");
            return;
        }
        // 安全检查：拒绝含shell元字符的命令，防注入
        const std::string forbidden = "&|;$`(){}<>";
        bool hasControlChar = false;
        for (unsigned char ch : cmd) {
            if (ch < 0x20 || ch == 0x7f) {
                hasControlChar = true;
                break;
            }
        }
        if (cmd.find_first_of(forbidden) != std::string::npos || hasControlChar) {
            res.set_content("ERROR: invalid characters in command", "text/plain");
            return;
        }
        // 只允许已知的mosaicraft子命令
        const std::string prefix = "mosaicraft ";
        if (cmd.compare(0, prefix.size(), prefix) != 0) {
            res.set_content("ERROR: command must start with 'mosaicraft'", "text/plain");
            return;
        }
        std::string subCmd = cmd.substr(prefix.size());
        // 提取子命令名（第一个空格前的词）
        auto spacePos = subCmd.find(' ');
        std::string cmdName = (spacePos != std::string::npos) ? subCmd.substr(0, spacePos) : subCmd;
        const std::string validCmds[] = {"build","mosaic","inspect","db-stats","db-purge","db-usage","db-health"};
        bool valid = false;
        for (const auto& vc : validCmds) { if (cmdName == vc) { valid = true; break; } }
        if (!valid) {
            res.set_content("ERROR: unknown command: " + cmdName, "text/plain");
            return;
        }

        // 使用 CreateProcess 替代 popen，避免 cmd.exe 对 Unicode 路径的编码问题
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

            // 确保命令行缓冲区足够大
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
            // Linux/macOS: popen 可以正确处理 UTF-8 路径
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

    // 健康检查
    svr.Get("/api/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("pong", "text/plain");
    });

    std::cout << "  Listening on http://localhost:" << port << std::endl;
    std::cout << "  Press Ctrl+C to stop" << std::endl;

    // 尝试打开浏览器
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
    // listen() 只在出错或 Ctrl+C 时返回
    std::cout << std::endl << "Server stopped." << std::endl;
    // 短暂暂停让用户看到停止信息（Windows 双击启动时控制台会立即关闭）
#ifdef _WIN32
    std::this_thread::sleep_for(std::chrono::seconds(3));
#endif
    return 0;
}
