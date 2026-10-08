#include "core/CandidateFeatures.h"
#include <iostream>
#include <sstream>
#include <unordered_set>

using namespace mosaicraft;

// 显式运行的读取对比工具；候选集合来自同一固定样本生成的校验元数据。
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
    std::ifstream metadata(u8path(directory + "/features.meta"));
    std::string line;
    std::getline(metadata, line);
    std::unordered_set<int> wanted;
    while (std::getline(metadata, line))
    {
        std::istringstream row(line);
        int id;
        if (row >> id)
        {
            wanted.insert(id);
        }
    }
    std::vector<int> candidates;
    for (size_t i = 0; i < records.size(); ++i)
    {
        if (wanted.count(records[i].id))
        {
            candidates.push_back(static_cast<int>(i));
        }
    }
    if (candidates.empty())
    {
        std::cerr << "No validated candidates; run a fixed CPU workload first.\n";
        return 1;
    }
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
                    std::ifstream t(record.tinyPath, std::ios::binary), l(record.histPath, std::ios::binary);
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
