#pragma once

#include "BuildService.h"
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
    Failed,
    Canceled
};

struct JobSnapshot
{
    std::string id;
    std::string type;
    JobState state = JobState::Queued;
    ServiceResult result;
    std::string inputPath;
    std::string outputPath;
    std::string requestJson;
    std::chrono::system_clock::time_point createdAt;
    std::chrono::system_clock::time_point startedAt;
    std::chrono::system_clock::time_point finishedAt;
};

class JobManager
{
public:
    explicit JobManager(bool autoStartWorker = true);
    ~JobManager();

    JobManager(const JobManager&) = delete;
    JobManager& operator=(const JobManager&) = delete;

    std::string submitMosaic(MosaicRequest request);
    std::string submitBuild(BuildRequest request);
    bool getJob(const std::string& id, JobSnapshot& out) const;
    bool waitJob(const std::string& id, JobSnapshot& out);
    std::vector<JobSnapshot> listJobs() const;
    bool cancelQueuedJob(const std::string& id, JobSnapshot& out);
    int clearFinishedJobs();
    bool getEvents(const std::string &id, uint64_t after, size_t limit, EventPage &out) const;
    const std::string &sessionId() const
    {
        return m_sessionId;
    }

private:
    struct JobRecord
    {
        JobSnapshot snapshot;
        std::shared_ptr<RuntimeContext> runtime = std::make_shared<RuntimeContext>();
        MosaicRequest request;
        BuildRequest buildRequest;
    };

    std::string nextId();
    void trimFinishedJobs();
    void workerLoop();
    void runMosaicJob(const std::string& id);
    void runBuildJob(const std::string& id);
    void finishJob(const std::string& id, const ServiceResult& result);

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::unordered_map<std::string, std::shared_ptr<JobRecord>> m_jobs;
    std::deque<std::string> m_queue;
    bool m_stopping = false;
    std::thread m_worker;
    uint64_t m_nextId = 1;
    std::string m_sessionId;
};

const char* jobStateName(JobState state);

} // namespace mosaicraft
