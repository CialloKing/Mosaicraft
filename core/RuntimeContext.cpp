#include "RuntimeContext.h"
#include "UnicodeIO.h"
#include <algorithm>
#include <iostream>

namespace mosaicraft
{
namespace
{
int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
size_t eventBytes(const Diagnostic &event)
{
    return event.code.size() + event.severity.size() + event.stage.size() + event.message.size() + event.field.size() +
           event.path.size() + sizeof(Diagnostic);
}
} // namespace

std::string truncateUtf8(const std::string &text, size_t bytes)
{
    if (text.size() <= bytes)
    {
        return text;
    }
    size_t end = bytes > 3 ? bytes - 3 : 0;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
    {
        --end;
    }
    return text.substr(0, end) + "...";
}

void RuntimeContext::stage(const std::string &name) noexcept
{
    try
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_snapshot.progress.stage == name)
            {
                return;
            }
            m_snapshot.progress = {name, {}, nowMs()};
            m_lastProgress.clear();
        }
        emit("info", "stage_started", name);
    }
    catch (...)
    {
    }
}

void RuntimeContext::advance(const std::string &id, uint64_t completed, int64_t total, const std::string &unit,
                             bool force) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto now = std::chrono::steady_clock::now();
        auto &last = m_lastProgress[id];
        if (!force && now - last < std::chrono::milliseconds(200))
        {
            return;
        }
        last = now;
        auto &items = m_snapshot.progress.activities;
        auto found = std::find_if(items.begin(), items.end(),
                                  [&](const auto &item)
                                  {
                                      return item.id == id;
                                  });
        if (found == items.end())
        {
            items.push_back({id, unit, completed, total});
        }
        else
        {
            found->completed = std::max(found->completed, completed);
            found->total = total;
        }
        m_snapshot.progress.updatedAt = nowMs();
    }
    catch (...)
    {
    }
}

void RuntimeContext::emit(const std::string &severity, const std::string &code, const std::string &message,
                          const std::string &field, const std::string &path, uint64_t count) noexcept
{
    try
    {
        Diagnostic event;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            event = {++m_sequence,
                     nowMs(),
                     truncateUtf8(code, 128),
                     severity,
                     m_snapshot.progress.stage,
                     truncateUtf8(message, 4096),
                     truncateUtf8(field, 128),
                     truncateUtf8(path, 4096)};
            if (severity == "error" && m_snapshot.diagnostic.code.empty())
            {
                m_snapshot.diagnostic = event;
            }
            if (severity == "warning")
            {
                m_snapshot.warningCount += count;
                // 原因码由生产端的有限集合提供；路径样例独立限量。
                auto &summary = m_snapshot.warnings[event.code];
                summary.count += count;
                summary.message = event.message;
                if (!event.path.empty() && summary.paths.size() < 3 &&
                    std::find(summary.paths.begin(), summary.paths.end(), event.path) == summary.paths.end())
                {
                    summary.paths.push_back(event.path);
                }
            }
            m_eventBytes += eventBytes(event);
            m_events.push_back(event);
            while (m_events.size() > 256 || m_eventBytes > 256 * 1024)
            {
                m_eventBytes -= eventBytes(m_events.front());
                m_events.pop_front();
                ++m_dropped;
            }
        }
        if (m_console && severity != "info")
        {
            std::cerr << (severity == "error" ? "ERROR: " : "WARN: ") << event.message << std::endl;
        }
    }
    catch (...)
    {
    }
}

bool RuntimeContext::fail(const std::string &code, const std::string &message, const std::string &field,
                          const std::string &path) noexcept
{
    emit("error", code, message, field, path);
    return false;
}

void RuntimeContext::backends(const std::string &features, const std::string &scoring,
                              const std::string &reason) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot.featureBackend = features;
        m_snapshot.scoreBackend = scoring;
        if (!reason.empty())
        {
            m_snapshot.fallbackReason = reason;
        }
    }
    catch (...)
    {
    }
}

void RuntimeContext::output(int width, int height, const std::string &format, int matched, int failed) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot.width = width;
        m_snapshot.height = height;
        m_snapshot.format = format;
        m_snapshot.matched = matched;
        m_snapshot.failed = failed;
    }
    catch (...)
    {
    }
}

void RuntimeContext::metric(const std::string &name, double value) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot.metrics[name] = value;
    }
    catch (...)
    {
    }
}

void RuntimeContext::artifact(const std::string &id, const std::string &kind, const std::string &path,
                              const std::string &mime) noexcept
{
    try
    {
        const auto canonical = std::filesystem::canonical(u8path(path));
        Artifact item{id, kind, pathToUtf8(canonical), mime};
        item.directory = std::filesystem::is_directory(canonical);
        if (!item.directory)
        {
            if (!std::filesystem::is_regular_file(canonical))
            {
                return;
            }
            item.bytes = std::filesystem::file_size(canonical);
            item.modified = std::filesystem::last_write_time(canonical);
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        auto &items = m_snapshot.artifacts;
        const auto found = std::find_if(items.begin(), items.end(),
                                        [&](const auto &old)
                                        {
                                            return old.id == id;
                                        });
        if (found == items.end())
        {
            items.push_back(std::move(item));
        }
        else
        {
            *found = std::move(item);
        }
    }
    catch (...)
    {
    }
}

RuntimeSnapshot RuntimeContext::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

EventPage RuntimeContext::events(uint64_t after, size_t limit) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    EventPage page;
    page.firstSequence = m_events.empty() ? m_sequence + 1 : m_events.front().sequence;
    page.dropped = m_dropped;
    page.truncated = after < page.firstSequence - 1;
    page.nextCursor = std::min(after, m_sequence);
    for (const auto &event : m_events)
    {
        if (event.sequence > after && page.events.size() < std::min<size_t>(100, limit))
        {
            page.events.push_back(event);
            page.nextCursor = event.sequence;
        }
    }
    return page;
}
} // namespace mosaicraft
