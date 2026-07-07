#include "JobManager.h"

#include <exception>
#include <iomanip>
#include <sstream>
#include "core/ConsoleGuard.h"

namespace mosaicraft
{

const char* jobStateName(JobState state)
{
    switch (state)
    {
    case JobState::Queued: return "queued";
    case JobState::Running: return "running";
    case JobState::Succeeded: return "succeeded";
    case JobState::Failed: return "failed";
    case JobState::Canceled: return "canceled";
    }
    return "unknown";
}

JobManager::~JobManager()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_cv.notify_all();
    if (m_worker.joinable())
    {
        m_worker.join();
    }
}

JobManager::JobManager(bool autoStartWorker)
{
    if (autoStartWorker)
    {
        m_worker = std::thread([this]() { workerLoop(); });
    }
}

std::string JobManager::submitMosaic(MosaicRequest request)
{
    auto record = std::make_shared<JobRecord>();
    record->request = std::move(request);
    record->snapshot.id = nextId();
    record->snapshot.type = "mosaic";
    record->snapshot.state = JobState::Queued;
    record->snapshot.inputPath = record->request.inputPath;
    record->snapshot.outputPath = record->request.outputPath;
    record->snapshot.createdAt = std::chrono::system_clock::now();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.emplace(record->snapshot.id, record);
        m_queue.push_back(record->snapshot.id);
    }
    m_cv.notify_one();

    return record->snapshot.id;
}

std::string JobManager::submitBuild(BuildRequest request)
{
    auto record = std::make_shared<JobRecord>();
    record->buildRequest = std::move(request);
    record->snapshot.id = nextId();
    record->snapshot.type = "build";
    record->snapshot.state = JobState::Queued;
    record->snapshot.inputPath = record->buildRequest.inputDir;
    record->snapshot.outputPath = record->buildRequest.outputDir;
    record->snapshot.createdAt = std::chrono::system_clock::now();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.emplace(record->snapshot.id, record);
        m_queue.push_back(record->snapshot.id);
    }
    m_cv.notify_one();

    return record->snapshot.id;
}

bool JobManager::getJob(const std::string& id, JobSnapshot& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_jobs.find(id);
    if (it == m_jobs.end()) return false;
    out = it->second->snapshot;
    return true;
}

bool JobManager::waitJob(const std::string& id, JobSnapshot& out)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this, &id]() {
        auto it = m_jobs.find(id);
        if (it == m_jobs.end()) return true;
        JobState state = it->second->snapshot.state;
        return state == JobState::Succeeded || state == JobState::Failed || state == JobState::Canceled;
    });

    auto it = m_jobs.find(id);
    if (it == m_jobs.end()) return false;
    out = it->second->snapshot;
    return true;
}

std::vector<JobSnapshot> JobManager::listJobs() const
{
    std::vector<JobSnapshot> jobs;
    std::lock_guard<std::mutex> lock(m_mutex);
    jobs.reserve(m_jobs.size());
    for (const auto& item : m_jobs)
    {
        jobs.push_back(item.second->snapshot);
    }
    return jobs;
}

bool JobManager::cancelQueuedJob(const std::string& id, JobSnapshot& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_jobs.find(id);
    if (it == m_jobs.end()) return false;

    auto& record = it->second;
    if (record->snapshot.state != JobState::Queued)
    {
        out = record->snapshot;
        return false;
    }

    for (auto q = m_queue.begin(); q != m_queue.end(); ++q)
    {
        if (*q == id)
        {
            m_queue.erase(q);
            break;
        }
    }

    record->snapshot.state = JobState::Canceled;
    record->snapshot.result = ServiceResult::failure(1, "job canceled");
    record->snapshot.finishedAt = std::chrono::system_clock::now();
    out = record->snapshot;
    m_cv.notify_all();
    return true;
}

int JobManager::clearFinishedJobs()
{
    int removed = 0;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_jobs.begin(); it != m_jobs.end(); )
    {
        JobState state = it->second->snapshot.state;
        if (state == JobState::Succeeded || state == JobState::Failed || state == JobState::Canceled)
        {
            it = m_jobs.erase(it);
            removed++;
        }
        else
        {
            ++it;
        }
    }
    return removed;
}

std::string JobManager::nextId()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ostringstream oss;
    oss << "job-" << std::setw(6) << std::setfill('0') << m_nextId++;
    return oss.str();
}

void JobManager::workerLoop()
{
    while (true)
    {
        std::string id;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() {
                return m_stopping || !m_queue.empty();
            });
            if (m_stopping && m_queue.empty()) return;
            id = m_queue.front();
            m_queue.pop_front();
        }

        std::string type;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_jobs.find(id);
            if (it != m_jobs.end()) type = it->second->snapshot.type;
        }

        if (type == "build") {
            ConsoleQuickEditGuard guard;
            runBuildJob(id);
        }
        else {
            ConsoleQuickEditGuard guard;
            runMosaicJob(id);
        }
    }
}

void JobManager::runMosaicJob(const std::string& id)
{
    MosaicRequest request;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_jobs.find(id);
        if (it == m_jobs.end()) return;
        auto& record = it->second;
        record->snapshot.state = JobState::Running;
        record->snapshot.startedAt = std::chrono::system_clock::now();
        request = record->request;
    }

    ServiceResult result;
    try
    {
        MosaicService service;
        result = service.run(request);
    }
    catch (const std::exception& e)
    {
        result = ServiceResult::failure(1, e.what());
    }
    catch (...)
    {
        result = ServiceResult::failure(1, "internal error");
    }

    finishJob(id, result);
}

void JobManager::runBuildJob(const std::string& id)
{
    BuildRequest request;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_jobs.find(id);
        if (it == m_jobs.end()) return;
        auto& record = it->second;
        record->snapshot.state = JobState::Running;
        record->snapshot.startedAt = std::chrono::system_clock::now();
        request = record->buildRequest;
    }

    ServiceResult result;
    try
    {
        BuildService service;
        result = service.run(request);
    }
    catch (const std::exception& e)
    {
        result = ServiceResult::failure(1, e.what());
    }
    catch (...)
    {
        result = ServiceResult::failure(1, "internal error");
    }

    finishJob(id, result);
}

void JobManager::finishJob(const std::string& id, const ServiceResult& result)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_jobs.find(id);
        if (it == m_jobs.end()) return;
        auto& record = it->second;
        record->snapshot.result = result;
        record->snapshot.state = result.ok ? JobState::Succeeded : JobState::Failed;
        record->snapshot.finishedAt = std::chrono::system_clock::now();
    }
    m_cv.notify_all();
}

} // namespace mosaicraft
