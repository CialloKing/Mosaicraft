#include "compute/CompatibleFeatures.h"
#include "core/FeatureExtractor.h"
#include "core/FeatureUtils.h"
#include "core/json.hpp"
#include <chrono>
#include <iostream>

using namespace mosaicraft;
using namespace mosaicraft::cuda;
using Clock = std::chrono::steady_clock;

// 只测特征阶段：同一批像素、线程数和批量，计时之外逐字段检查结果。
static CompatibleFeature cpuFeatures(const cv::Mat &image, ImageRecord *record)
{
    CompatibleFeature result;
    cv::Mat lab, gray;
    cv::cvtColor(image, lab, cv::COLOR_BGR2Lab);
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    const auto mean = cv::mean(lab);
    std::copy(mean.val, mean.val + 3, result.lab.begin());
    const auto grid = computeGrid8x8FromLab(lab);
    const auto tiny = computeTinyImageFromGray(gray);
    const auto lbp = computeLBPHistogramFromGray(gray);
    std::copy(grid.begin(), grid.end(), result.grid.begin());
    std::copy(tiny.begin(), tiny.end(), result.tiny.begin());
    std::copy(lbp.begin(), lbp.end(), result.lbp.begin());
    if (record)
    {
        FeatureExtractor::computeStatistics(image, *record, lab, gray);
        result.edge = record->edgeDensity;
    }
    else
    {
        result.edge = computeEdgeDensityFromGray(gray);
    }
    return result;
}

int main()
{
    if (!isCudaAvailable())
    {
        std::cerr << "a real CUDA device is required\n";
        return 1;
    }
    nlohmann::json report = nlohmann::json::array();
    for (int side : {256, 512})
    {
        for (bool statistics : {false, true})
        {
            const unsigned threads = statistics ? 8 : 16;
            WorkerPool workers(threads);
            CompatibleFeatureWorkspace workspace;
            if (!workspace.prepare(side, side, 256, (statistics ? 64ULL : 256ULL) * 1024 * 1024, statistics))
            {
                return 2;
            }
            const size_t count = 512;
            std::vector<cv::Mat> images(count);
            cv::RNG random(123);
            for (auto &image : images)
            {
                image.create(side, side, CV_8UC3);
                random.fill(image, cv::RNG::UNIFORM, 0, 256);
            }
            std::vector<CompatibleFeature> expected(count), actual(count);
            std::vector<ImageRecord> cpuRecords(count), gpuRecords(count);
            for (int run = 0; run <= 3; ++run)
            {
                for (int order = 0; order < 2; ++order)
                {
                    const bool gpu = (run + order) % 2 != 0;
                    CompatibleTimings timings;
                    const auto start = Clock::now();
                    for (size_t first = 0; first < count; first += workspace.capacity())
                    {
                        const size_t batch = std::min<size_t>(workspace.capacity(), count - first);
                        if (gpu)
                        {
                            std::vector<cv::Mat> input(images.begin() + first, images.begin() + first + batch);
                            std::vector<ImageRecord *> records;
                            if (statistics)
                            {
                                for (size_t i = 0; i < batch; ++i)
                                {
                                    records.push_back(&gpuRecords[first + i]);
                                }
                            }
                            std::vector<CompatibleFeature> output;
                            workspace.extract(input, output, workers, records, &timings);
                            std::copy(output.begin(), output.end(), actual.begin() + first);
                        }
                        else
                        {
                            workers.run(batch, [&](size_t i)
                            {
                                expected[first + i] = cpuFeatures(images[first + i], statistics ? &cpuRecords[first + i] : nullptr);
                            });
                        }
                    }
                    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                    report.push_back({{"side", side}, {"statistics", statistics}, {"threads", threads},
                        {"images", count}, {"batch_capacity", workspace.capacity()}, {"run", run},
                        {"warmup", run == 0}, {"backend", gpu ? "generic_cuda" : "cpu"}, {"wall_ms", ms},
                        {"prepare_ms", timings.prepareMs}, {"cpu_completion_ms", timings.cpuMs},
                        {"upload_ms", timings.gpu.eventUploadMs}, {"kernel_ms", timings.gpu.eventComputeMs},
                        {"download_ms", timings.gpu.eventDownloadMs}, {"wait_ms", timings.gpu.waitMs},
                        {"host_buffer_bytes", workspace.hostBytes()}, {"device_buffer_bytes", workspace.deviceBytes()}});
                }
                for (size_t i = 0; i < count; ++i)
                {
                    const auto &a = expected[i], &b = actual[i];
                    const auto &c = cpuRecords[i], &d = gpuRecords[i];
                    if (a.lab != b.lab || a.grid != b.grid || a.tiny != b.tiny || a.lbp != b.lbp || a.edge != b.edge ||
                        c.meanBrightness != d.meanBrightness || c.stdBrightness != d.stdBrightness ||
                        c.contrast != d.contrast || c.saturation != d.saturation || c.colorVariance != d.colorVariance)
                    {
                        std::cerr << "CPU/CUDA feature mismatch\n";
                        return 3;
                    }
                }
            }
        }
    }
    std::cout << report.dump(2) << '\n';
}
