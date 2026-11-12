#pragma once

#include "json.hpp"
#include <mutex>

struct sqlite3;

namespace mosaicraft
{
struct JobSnapshot;

class HistoryStore
{
public:
    HistoryStore() noexcept;
    ~HistoryStore();
    bool available() const noexcept { return m_db != nullptr; }
    bool record(const std::string &session, const JobSnapshot &snapshot) noexcept;
    bool list(nlohmann::json &records) const;
    bool clear(int &removed);

private:
    mutable std::mutex m_mutex;
    sqlite3 *m_db = nullptr;
};
} // namespace mosaicraft
