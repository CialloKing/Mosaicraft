#pragma once
#include "ColorDistribution.h"
#include <memory>
#include <chrono>
#include "OutputPlan.h"
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mosaicraft
{
struct Diagnostic
{
    uint64_t sequence = 0;
    int64_t timestamp = 0;
    std::string code, severity, stage, message, field, path;
};

struct ProgressActivity
{
    std::string id, unit;
    uint64_t completed = 0;
    int64_t total = -1;
};

struct ProgressSnapshot
{
    std::string stage;
    std::vector<ProgressActivity> activities;
    int64_t updatedAt = 0;
    int64_t startedAt = 0;
};

struct Artifact
{
    std::string id, kind, path, mime;
    uint64_t bytes = 0;
    std::filesystem::file_time_type modified{};
    bool directory = false;
};

struct WarningSummary
{
    uint64_t count = 0;
    std::string message;
    std::vector<std::string> paths;
};

struct RuntimeSnapshot
{
    ProgressSnapshot progress;
    Diagnostic diagnostic;
    uint64_t warningCount = 0;
    std::map<std::string, WarningSummary> warnings;
    std::string featureBackend, scoreBackend, fallbackReason, format;
    int width = 0, height = 0, matched = 0, failed = 0;
    std::map<std::string, double> metrics;
    std::vector<Artifact> artifacts;
    std::optional<detail::OutputPlan> outputPlan;
    std::shared_ptr<const BuildVisuals> buildVisuals;
};

struct EventPage
{
    std::vector<Diagnostic> events;
    uint64_t firstSequence = 0, nextCursor = 0, dropped = 0;
    bool truncated = false;
};

// 一次执行独占上下文；线程只更新有界内存，HTTP 读取不进入业务管线。
class RuntimeContext
{
  public:
    explicit RuntimeContext(bool console = true) : m_console(console) {}
    void stage(const std::string &name) noexcept;
    void advance(const std::string &id, uint64_t completed, int64_t total, const std::string &unit,
                 bool force = false) noexcept;
    void emit(const std::string &severity, const std::string &code, const std::string &message,
              const std::string &field = {}, const std::string &path = {}, uint64_t count = 1) noexcept;
    bool fail(const std::string &code, const std::string &message, const std::string &field = {},
              const std::string &path = {}) noexcept;
    void backends(const std::string &features, const std::string &scoring, const std::string &reason = {}) noexcept;
    void output(int width, int height, const std::string &format, int matched, int failed) noexcept;
    void planOutput(const detail::OutputPlan &plan, const std::string &requestedPath) noexcept;
    void metric(const std::string &name, double value) noexcept;
    void buildVisuals(BuildVisuals data) noexcept;
    void artifact(const std::string &id, const std::string &kind, const std::string &path,
                  const std::string &mime = "application/octet-stream") noexcept;
    RuntimeSnapshot snapshot() const;
    EventPage events(uint64_t after, size_t limit = 100) const;

  private:
    bool m_console;
    mutable std::mutex m_mutex;
    RuntimeSnapshot m_snapshot;
    std::deque<Diagnostic> m_events;
    size_t m_eventBytes = 0;
    uint64_t m_sequence = 0, m_dropped = 0;
    std::map<std::string, std::chrono::steady_clock::time_point> m_lastProgress;
};

std::string truncateUtf8(const std::string &text, size_t bytes);

// 解析器在读取字段时记录字段名，避免从面向人的英文错误中反推字段。
struct RequestError
{
    std::string message, field;
    bool empty() const
    {
        return message.empty();
    }
    RequestError &operator=(const std::string &value)
    {
        message = value;
        return *this;
    }
    operator const std::string &() const
    {
        return message;
    }
};
} // namespace mosaicraft
