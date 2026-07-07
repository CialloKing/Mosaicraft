#pragma once

#include "ImageNormalizer.h"

#include <string>
#include <vector>

namespace mosaicraft
{

// BatchProcessor — 批量归一化整个目录的图片。
// 支持多线程并行处理，输出到统一目录，保持原文件名。
class BatchProcessor
{
public:
    // normalizer 必须生存期长于 BatchProcessor 的 process() 调用
    explicit BatchProcessor(ImageNormalizer& normalizer, int threadCount = 0);

    // 扫描 inputDir 下所有图片，归一化后写入 outputDir
    // 返回成功处理的数量；threadCount=0 表示自动检测 CPU 核数
    int process(const std::string& inputDir, const std::string& outputDir);

private:
    ImageNormalizer& m_normalizer;
    int m_threadCount;

    // 支持的图片扩展名（小写）
    static const std::vector<std::string>& supportedExtensions();

    // 收集目录下所有匹配的图片路径
    std::vector<std::string> collectFiles(const std::string& dir);

    // 单线程处理一批文件
    int processFiles(const std::vector<std::string>& files,
                     const std::string& outputDir);
};

} // namespace mosaicraft
