#pragma once
#include "RuntimeContext.h"
#include <filesystem>
#include <functional>
#include <string>

namespace mosaicraft
{
bool pathWithin(const std::filesystem::path &path, const std::filesystem::path &root);
void protectOutput(const std::string &output, const std::vector<std::string> &inputs);

// 暂存和备份放在同一文件系统内；失败时旧输出仍可恢复。
class OutputPublication
{
  public:
    OutputPublication(const std::string &destination, bool directory, bool overwrite,
                      std::vector<std::string> protectedInputs = {});
    ~OutputPublication();
    std::string workPath() const;
    void commit(RuntimeContext &runtime, const std::function<void()> &beforeFinalize = {});

  private:
    std::filesystem::path destination, staging;
    std::vector<std::string> protectedInputs;
    bool directory, overwrite, committed = false;
};
} // namespace mosaicraft
