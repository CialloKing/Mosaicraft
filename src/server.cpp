// Mosaicraft Web UI — 本地 HTTP 服务器
// 提供命令生成页面 + 后端执行 mosaicraft.exe
#include <httplib.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#define popen _popen
#define pclose _pclose
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
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
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
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path candidate = exeDir / "mosaicraft.exe";
    if (std::filesystem::exists(candidate)) return candidate.string();
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
    if (argc > 1) port = std::atoi(argv[1]);

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

    httplib::Server svr;

    // 长任务超时设置（建库可能需要数分钟）
    svr.set_read_timeout(1800);   // 30 min
    svr.set_write_timeout(1800);

    // 主页
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(htmlContent, "text/html; charset=utf-8");
    });

    // 重启时重新加载 HTML（开发用）
    svr.Get("/reload", [&](const httplib::Request&, httplib::Response& res) {
        htmlContent = readFile(htmlPath);
        res.set_content("HTML reloaded", "text/plain");
    });

    // 执行命令
    svr.Post("/api/run", [&](const httplib::Request& req, httplib::Response& res) {
        std::string cmd = req.body;
        if (cmd.empty()) {
            res.set_content("ERROR: empty command", "text/plain");
            return;
        }
        // 确保使用绝对路径的 mosaicraft
        std::string fullCmd = "\"" + mosaicPath + "\" ";
        // Web 端命令已包含 "mosaicraft" 前缀，去掉重复
        const std::string prefix = "mosaicraft ";
        if (cmd.compare(0, prefix.size(), prefix) == 0)
            fullCmd += cmd.substr(prefix.size());
        else
            fullCmd += cmd;
        std::cout << "[RUN] " << fullCmd << std::endl;

        std::string output;
#ifdef _WIN32
        // Windows: 用 CreateProcess 获取实时输出（简单起见用 popen）
        FILE* pipe = popen((fullCmd + " 2>&1").c_str(), "r");
#else
        FILE* pipe = popen((fullCmd + " 2>&1").c_str(), "r");
#endif
        if (!pipe) {
            res.set_content("ERROR: failed to start process", "text/plain");
            return;
        }
        // 用 fread 逐字节读取——mosaicraft 进度用 \r 而非 \n，fgets 不触发
        setvbuf(pipe, nullptr, _IONBF, 0);
        char ch;
        while (fread(&ch, 1, 1, pipe) == 1) {
            std::cout << ch;      // 逐字输出到控制台
            output += ch;
        }
        int rc = pclose(pipe);

        if (rc == 0)
            res.set_content(output.empty() ? "OK" : output, "text/plain; charset=utf-8");
        else
            res.set_content("EXIT " + std::to_string(rc) + "\n" + output, "text/plain; charset=utf-8");
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
    std::this_thread::sleep_for(std::chrono::seconds(30));
    return 0;
}
