#include "BatchProcessor.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace mosaicraft
{

namespace fs = std::filesystem;

BatchProcessor::BatchProcessor(ImageNormalizer& normalizer, int threadCount)
    : m_normalizer(normalizer)
    , m_threadCount(threadCount)
{
    // 自动检测 CPU 核数；至少用 1 个线程
    if (m_threadCount <= 0)
    {
        m_threadCount = static_cast<int>(std::thread::hardware_concurrency());
        if (m_threadCount <= 0)
        {
            m_threadCount = 1;
        }
    }
}

int BatchProcessor::process(const std::string& inputDir, const std::string& outputDir)
{
    // 1. 收集所有图片文件
    std::vector<std::string> files = collectFiles(inputDir);
    if (files.empty())
    {
        std::cerr << "No supported images found in: " << inputDir << std::endl;
        return 0;
    }

    std::cout << "Found " << files.size() << " image(s) in " << inputDir << std::endl;

    // 2. 确保输出目录存在
    std::error_code ec;
    fs::create_directories(outputDir, ec);
    if (ec)
    {
        std::cerr << "Cannot create output directory: " << outputDir << std::endl;
        return 0;
    }

    // 3. 多线程处理
    int total = processFiles(files, outputDir);

    std::cout << "Done: " << total << " / " << files.size() << " processed" << std::endl;
    return total;
}

std::vector<std::string> BatchProcessor::collectFiles(const std::string& dir)
{
    std::vector<std::string> files;
    const auto& exts = supportedExtensions();

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        std::string ext = entry.path().extension().string();
        // 转小写比较
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (std::find(exts.begin(), exts.end(), ext) != exts.end())
        {
            files.push_back(entry.path().string());
        }
    }

    // 按路径排序，保证输出顺序可预期
    std::sort(files.begin(), files.end());
    return files;
}

int BatchProcessor::processFiles(const std::vector<std::string>& files,
                                  const std::string& outputDir)
{
    const int n = static_cast<int>(files.size());
    // 实际线程数不超过文件数
    const int numThreads = std::min(m_threadCount, n);

    // 每个线程的结果计数
    std::vector<int> results(static_cast<std::size_t>(numThreads), 0);
    std::vector<std::thread> threads;

    for (int t = 0; t < numThreads; ++t)
    {
        threads.emplace_back(
            [this, &files, &outputDir, &results, t, numThreads, n]()
            {
                // 按轮询方式分配文件：线程 t 处理索引 t, t+numThreads, t+2*numThreads, ...
                // 这样大文件和目录混合时负载更均衡
                for (int i = t; i < n; i += numThreads)
                {
                    const std::string& inputPath = files[static_cast<std::size_t>(i)];

                    // 构造输出路径：保留原文件名
                    fs::path outPath = fs::path(outputDir) / fs::path(inputPath).filename();

                    if (m_normalizer.process(inputPath, outPath.string()))
                    {
                        ++results[static_cast<std::size_t>(t)];
                    }
                }
            });
    }

    for (auto& th : threads)
    {
        th.join();
    }

    int total = 0;
    for (int r : results)
    {
        total += r;
    }
    return total;
}

const std::vector<std::string>& BatchProcessor::supportedExtensions()
{
    // 静态常量：程序生命周期内只初始化一次
    static const std::vector<std::string> exts = {
        ".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tiff", ".tif"
    };
    return exts;
}

} // namespace mosaicraft
