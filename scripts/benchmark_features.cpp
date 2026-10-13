#include "core/CandidateFeatures.h"
#include "core/UnicodeIO.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>

using namespace mosaicraft;

// 显式比较同一数据库全库特征的逐文件读取与经过校验的包读取。
int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: benchmark_features <snapshot.db> <features directory>\n";
        return 1;
    }
    Database database(argv[1]);
    const auto records = database.allRecords();
    const std::string directory = argv[2];
    std::vector<int> candidates(records.size());
    std::iota(candidates.begin(), candidates.end(), 0);
    std::cout << "candidates=" << candidates.size() << '\n';
    uint64_t expected = 0;
    for (int run = 0; run < 4; ++run)
    {
        for (int order = 0; order < 2; ++order)
        {
            const bool packed = (order + run) % 2 != 0;
            uint64_t checksum = 0;
            const auto start = std::chrono::steady_clock::now();
            if (packed)
            {
                CandidateFeatures features(records, candidates, directory);
                for (int index : candidates)
                {
                    if (features.hasTiny(index) && features.hasLbp(index))
                    {
                        checksum += features.tiny(index)[0];
                    }
                }
            }
            else
            {
                std::unordered_map<int, std::vector<uint8_t>> tiny;
                std::unordered_map<int, std::vector<float>> lbp;
                for (int index : candidates)
                {
                    const auto &record = records[index];
                    std::ifstream t(u8path(record.tinyPath), std::ios::binary),
                        l(u8path(record.histPath), std::ios::binary);
                    std::vector<uint8_t> td(256);
                    std::vector<float> ld(256);
                    if (t.read(reinterpret_cast<char *>(td.data()), 256) &&
                        l.read(reinterpret_cast<char *>(ld.data()), 1024))
                    {
                        checksum += td[0];
                        tiny.emplace(record.id, std::move(td));
                        lbp.emplace(record.id, std::move(ld));
                    }
                }
            }
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (run == 0 && order == 0)
            {
                expected = checksum;
            }
            if (checksum != expected)
            {
                std::cerr << "feature checksum mismatch\n";
                return 2;
            }
            std::cout << run << ' ' << (packed ? "packed" : "files") << ' ' << ms << '\n';
        }
    }
}
