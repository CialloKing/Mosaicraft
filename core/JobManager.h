#pragma once

#include "MosaicService.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mosaicraft
{

enum class JobState
{
    Queued,
    Running,
    Succeeded,
    Failed
};

struct JobSnapshot
{
    std::string id;
    std::string type;
    JobState state = JobState::Queued;
    ServiceResult result;
    std::string inputPath;
    std::string outputPath;
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point startedAt;
    std::chrono::system_clock::time_point finishedAt;
};

class JobManager
{
public:
    JobManager();
    ~JobManager();

    JobManager(const JobManager&) = delete;
    JobManager& operator=(const JobManager&) = delete;

    std::string submitMosaic(MosaicRequest request);
    bool getJob(const std::string& id, JobSnapshot& out) const;
    bool waitJob(const std::string& id, JobSnapshot& out);
    std::vector<JobSnapshot> listJobs() const;

private:
    struct JobRecord
    {
        JobSnapshot snapshot;
        MosaicRequest request;
    };

    std::string nextId();
    void workerLoop();
    void runMosaicJob(const std::string& id);

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::unordered_map<std::string, std::shared_ptr<JobRecord>> m_jobs;
    std::deque<std::string> m_queue;
    bool m_stopping = false;
    std::thread m_worker;
    uint64_t m_nextId = 1;
};

const char* jobStateName(JobState state);

} // namespace mosaicraft
