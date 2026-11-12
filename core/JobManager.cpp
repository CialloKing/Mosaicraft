#include "JobManager.h"
#include "ApiJson.h"
#include "ApiMetadata.h"
#include "HistoryStore.h"
#include <algorithm>
#include <random>

#include <exception>
#include <iomanip>
#include <iostream>
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
        if (m_runningPreview)
        {
            m_runningPreview->canceled = true;
        }
    }
    m_cv.notify_all();
    if (m_worker.joinable())
    {
        m_worker.join();
    }
}

JobManager::JobManager(bool autoStartWorker, bool persistentHistory)
{
    std::random_device random;
    std::ostringstream session;
    session << std::hex;
    for (int i = 0; i < 4; ++i)
    {
        session << std::setw(8) << std::setfill('0') << random();
    }
    m_sessionId = session.str();
    if (persistentHistory)
    {
        m_history = std::make_unique<HistoryStore>();
    }
    if (autoStartWorker)
    {
        m_worker = std::thread([this]() { workerLoop(); });
    }
}

std::string JobManager::submitMosaic(MosaicRequest request, std::shared_ptr<const MosaicSelection> selection)
{
    auto record = std::make_shared<JobRecord>();
    record->request = std::move(request);
    record->selection = std::move(selection);
    record->snapshot.id = nextId();
    record->snapshot.type = "mosaic";
    record->snapshot.requestJson = mosaicRequestToJson(record->request).dump();
    record->runtime->stage("queued");
    record->snapshot.state = JobState::Queued;
    record->snapshot.inputPath = record->request.inputPath;
    record->snapshot.outputPath = record->request.outputPath;
    record->snapshot.createdAt = std::chrono::system_clock::now();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.emplace(record->snapshot.id, record);
        m_queue.push_back(record->snapshot.id);
        interruptPreview();
    }
    m_cv.notify_one();

    return record->snapshot.id;
}

std::string JobManager::submitPreview(MosaicRequest request, uint32_t seed,
                                      std::shared_ptr<const MosaicSelection> source,
                                      std::shared_ptr<const MosaicSelection> compareRight)
{
    auto job = std::make_shared<PreviewJob>();
    job->snapshot.request = std::move(request);
    job->snapshot.seed = seed;
    job->source = std::move(source);
    job->compareRight = std::move(compareRight);
    job->runtime->stage("queued");
    job->queuedAt = job->accessed = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        trimPreviews();
        job->snapshot.id = m_sessionId + "-" + std::to_string(m_nextPreviewId++);
        if (m_pendingPreview)
        {
            m_previews.erase(m_pendingPreview->snapshot.id);
        }
        if (m_runningPreview)
        {
            m_runningPreview->interrupted = false;
            m_runningPreview->canceled = true;
        }
        m_previews.emplace(job->snapshot.id, job);
        m_pendingPreview = job;
    }
    m_cv.notify_one();
    return job->snapshot.id;
}

bool JobManager::getPreview(const std::string &id, PreviewSnapshot &out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    trimPreviews();
    const auto it = m_previews.find(id);
    if (it == m_previews.end())
    {
        return false;
    }
    it->second->accessed = std::chrono::steady_clock::now();
    out = it->second->snapshot;
    const auto runtime = it->second->runtime->snapshot();
    out.progress = runtime.progress;
    // Completed operations retain their diagnostics, including sampling and comparison failures.
    if (out.state == JobState::Queued || out.state == JobState::Running || out.state == JobState::Canceled)
    {
        out.result.runtime = runtime;
    }
    return true;
}

bool JobManager::closePreview(const std::string &id)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_previews.find(id);
        if (it == m_previews.end())
        {
            return false;
        }
        if (std::find(m_pinnedPreviews.begin(), m_pinnedPreviews.end(), id) != m_pinnedPreviews.end())
        {
            return true;
        }
        it->second->canceled = true;
        if (m_pendingPreview == it->second)
        {
            m_pendingPreview.reset();
        }
        m_previews.erase(it);
        m_releasePreviewWorkspace = m_previews.empty();
    }
    m_cv.notify_one();
    return true;
}

bool JobManager::pinPreview(int slot, const std::string &id)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    if (slot < 0 || slot >= static_cast<int>(m_pinnedPreviews.size()))
    {
        return false;
    }
    if (!id.empty())
    {
        const auto found = m_previews.find(id);
        if (found == m_previews.end() || !found->second->snapshot.selection)
        {
            return false;
        }
    }
    m_pinnedPreviews[slot] = id;
    trimPreviews();
    return true;
}

void JobManager::trimPreviews()
{
    const auto cutoff = std::chrono::steady_clock::now() - std::chrono::minutes(10);
    std::vector<std::shared_ptr<PreviewJob>> finished;
    size_t pinned = 0;
    for (auto it = m_previews.begin(); it != m_previews.end();)
    {
        const auto &job = it->second;
        if (job == m_pendingPreview || job == m_runningPreview)
        {
            ++it;
        }
        else if (std::find(m_pinnedPreviews.begin(), m_pinnedPreviews.end(), it->first) != m_pinnedPreviews.end())
        {
            ++pinned;
            ++it;
        }
        else if (job->accessed < cutoff)
        {
            it = m_previews.erase(it);
        }
        else
        {
            finished.push_back(job);
            ++it;
        }
    }
    std::sort(finished.begin(), finished.end(), [](const auto &left, const auto &right)
    {
        return left->accessed > right->accessed;
    });
    for (size_t i = 4 - pinned; i < finished.size(); ++i)
    {
        m_previews.erase(finished[i]->snapshot.id);
    }
}

void JobManager::runPreview(const std::shared_ptr<PreviewJob> &job)
{
    ConsoleQuickEditGuard guard;
    const auto started = std::chrono::steady_clock::now();
    const auto queuedMs = std::chrono::duration_cast<std::chrono::milliseconds>(started - job->queuedAt).count();
    std::cout << ("[Preview] started " + job->snapshot.id + " (queued for " + std::to_string(queuedMs) + " ms)\n")
              << std::flush;
    std::shared_ptr<MosaicSelection> selection;
    ServiceResult result;
    FileSizeSample fileSample;
    std::shared_ptr<PreviewComparison> comparison;
    try
    {
        if (job->compareRight)
        {
            comparison = std::make_shared<PreviewComparison>();
            result = comparePreview(job->snapshot.request, *job->source, *job->compareRight, job->canceled, *comparison);
        }
        else if (job->source)
        {
            result = sampleMosaicSize(job->snapshot.request, *job->source, job->canceled, fileSample);
        }
        else
        {
            selection = std::make_shared<MosaicSelection>();
            result = MosaicService().preview(job->snapshot.request, job->snapshot.seed, m_previewWorkspace, job->canceled, *selection,
                                             *job->runtime);
        }
    }
    catch (const std::exception &error)
    {
        result = ServiceResult::failure(1, error.what());
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (job->interrupted && !m_pendingPreview && !m_stopping &&
            m_previews.find(job->snapshot.id) != m_previews.end())
        {
            // 用户仍在等待这份预览，正式任务结束后继续处理。
            job->interrupted = false;
            job->canceled = false;
            job->snapshot.state = JobState::Queued;
            job->runtime->stage("queued");
            job->queuedAt = std::chrono::steady_clock::now();
            m_pendingPreview = job;
        }
        else if (job->canceled)
        {
            job->snapshot.state = JobState::Canceled;
        }
        else
        {
            job->snapshot.state = result.ok ? JobState::Succeeded : JobState::Failed;
            if (result.ok)
            {
                job->snapshot.selection = std::move(selection);
                if (comparison)
                {
                    job->snapshot.comparison = std::move(comparison);
                }
                else if (job->source)
                {
                    job->snapshot.fileSample = fileSample;
                }
            }
            job->snapshot.result = std::move(result);
            job->source.reset();
            job->compareRight.reset();
        }
        job->accessed = std::chrono::steady_clock::now();
        m_runningPreview.reset();
        trimPreviews();
    }

    // 控制台可能被重定向，写日志不能阻塞 HTTP 读取任务状态。
    const auto &snapshot = job->snapshot;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    std::ostringstream summary;
    summary << "[Preview] " << jobStateName(snapshot.state) << ' ' << snapshot.id << " after " << elapsed << " ms";
    if (snapshot.fileSample)
    {
        summary << " (" << snapshot.fileSample->regions << " sampled regions)";
    }
    else if (snapshot.state == JobState::Succeeded)
    {
        summary << " (" << snapshot.result.runtime.matched << " matched, " << snapshot.result.runtime.failed
                << " without candidates)";
    }
    else if (snapshot.state == JobState::Failed)
    {
        if (!snapshot.result.runtime.diagnostic.code.empty())
        {
            summary << " [" << snapshot.result.runtime.diagnostic.code << ']';
        }
        summary << ": " << snapshot.result.message;
    }
    else if (snapshot.state == JobState::Queued)
    {
        summary << " (resumes after the foreground job)";
    }
    summary << '\n';
    std::cout << summary.str() << std::flush;
}

void JobManager::interruptPreview()
{
    if (m_runningPreview && !m_runningPreview->canceled)
    {
        m_runningPreview->interrupted = true;
        m_runningPreview->canceled = true;
    }
}

std::string JobManager::submitBuild(BuildRequest request)
{
    auto record = std::make_shared<JobRecord>();
    record->buildRequest = std::move(request);
    record->snapshot.id = nextId();
    record->snapshot.type = "build";
    record->snapshot.requestJson = buildRequestToJson(record->buildRequest).dump();
    record->runtime->stage("queued");
    record->snapshot.state = JobState::Queued;
    record->snapshot.inputPath = record->buildRequest.inputDir;
    record->snapshot.outputPath = record->buildRequest.outputDir;
    record->snapshot.createdAt = std::chrono::system_clock::now();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.emplace(record->snapshot.id, record);
        m_queue.push_back(record->snapshot.id);
        interruptPreview();
    }
    m_cv.notify_one();

    return record->snapshot.id;
}

std::string JobManager::submitProject(const std::string &type, ProjectRequest request)
{
    if (type != "render" && type != "project-check" && type != "project-pack")
    {
        throw std::invalid_argument("unsupported project task");
    }
    auto record = std::make_shared<JobRecord>();
    record->projectRequest = std::move(request);
    record->snapshot.id = nextId();
    record->snapshot.type = type;
    auto fields = projectRequestToJson(record->projectRequest);
    const auto operation = type == "render"          ? ApiOperation::SubmitRenderJob
                           : type == "project-check" ? ApiOperation::SubmitProjectCheckJob
                                                     : ApiOperation::SubmitProjectPackJob;
    for (const auto &endpoint : apiEndpointMetadata())
    {
        if (endpoint.operation == operation)
        {
            for (auto it = fields.begin(); it != fields.end();)
            {
                if (std::find(endpoint.requestFields.begin(), endpoint.requestFields.end(), it.key()) ==
                    endpoint.requestFields.end())
                {
                    it = fields.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }
    record->snapshot.requestJson = fields.dump();
    record->snapshot.inputPath = record->projectRequest.projectPath;
    record->snapshot.outputPath = record->projectRequest.outputPath;
    record->snapshot.createdAt = std::chrono::system_clock::now();
    record->runtime->stage("queued");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.emplace(record->snapshot.id, record);
        m_queue.push_back(record->snapshot.id);
        interruptPreview();
    }
    m_cv.notify_one();
    return record->snapshot.id;
}

void JobManager::runProjectJob(const std::string &id)
{
    std::shared_ptr<JobRecord> record;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_jobs.find(id);
        if (it == m_jobs.end())
        {
            return;
        }
        record = it->second;
    }
    ServiceResult result;
    try
    {
        auto &runtime = *record->runtime;
        TaskResourceMonitor monitor(runtime, record->projectRequest.resourceMonitor);
        ProjectService service;
        result = record->snapshot.type == "render"          ? service.render(record->projectRequest, runtime)
                 : record->snapshot.type == "project-check" ? service.check(record->projectRequest, runtime)
                                                            : service.pack(record->projectRequest, runtime);
    }
    catch (const std::exception &error)
    {
        result = ServiceResult::failure(1, error.what());
    }
    finishJob(id, runtimeResult(std::move(result), *record->runtime));
}

bool JobManager::getJob(const std::string &id, JobSnapshot &out) const
{
    std::shared_ptr<RuntimeContext> runtime;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_jobs.find(id);
        if (found == m_jobs.end())
        {
            return false;
        }
        out = found->second->snapshot;
        runtime = found->second->runtime;
    }
    out.result.runtime = runtime->snapshot();
    return true;
}

bool JobManager::getEvents(const std::string &id, uint64_t after, size_t limit, EventPage &out) const
{
    std::shared_ptr<RuntimeContext> runtime;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto found = m_jobs.find(id);
        if (found == m_jobs.end())
        {
            return false;
        }
        runtime = found->second->runtime;
    }
    out = runtime->events(after, limit);
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
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto &entry : m_jobs)
        {
            ids.push_back(entry.first);
        }
    }
    std::vector<JobSnapshot> jobs;
    for (const auto &id : ids)
    {
        JobSnapshot snapshot;
        if (getJob(id, snapshot))
        {
            snapshot.requestJson.clear();
            jobs.push_back(std::move(snapshot));
        }
    }
    std::sort(jobs.begin(), jobs.end(),
              [](const auto &left, const auto &right)
              {
                  return left.createdAt != right.createdAt ? left.createdAt > right.createdAt : left.id > right.id;
              });
    return jobs;
}

void JobManager::trimFinishedJobs()
{
    // 调用方持有任务锁；只移除内存记录，不访问或删除产物。
    std::vector<std::shared_ptr<JobRecord>> finished;
    for (const auto &entry : m_jobs)
    {
        const auto state = entry.second->snapshot.state;
        if (state != JobState::Queued && state != JobState::Running)
        {
            finished.push_back(entry.second);
        }
    }
    std::sort(finished.begin(), finished.end(),
              [](const auto &left, const auto &right)
              {
                  return left->snapshot.finishedAt > right->snapshot.finishedAt;
              });
    for (size_t i = 50; i < finished.size(); ++i)
    {
        m_jobs.erase(finished[i]->snapshot.id);
    }
}

bool JobManager::cancelQueuedJob(const std::string& id, JobSnapshot& out)
{
    std::unique_lock<std::mutex> lock(m_mutex);
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
    record->selection.reset();
    record->snapshot.result = ServiceResult::failure(1, "job canceled");
    record->snapshot.finishedAt = std::chrono::system_clock::now();
    record->runtime->stage("canceled");
    record->snapshot.result.runtime = record->runtime->snapshot();
    out = record->snapshot;
    trimFinishedJobs();
    if (m_history && m_history->available() && !m_history->record(m_sessionId, out))
    {
        std::cerr << "WARNING: cannot save job history for " << id << '\n';
    }
    lock.unlock();
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
        std::shared_ptr<PreviewJob> preview;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            const bool work = m_cv.wait_for(lock, std::chrono::minutes(1), [this]() {
                return m_stopping || !m_queue.empty() || m_pendingPreview || m_releasePreviewWorkspace;
            });
            if (!work)
            {
                trimPreviews();
                m_releasePreviewWorkspace = m_previews.empty();
            }
            if (m_stopping && m_queue.empty()) return;
            if (m_releasePreviewWorkspace)
            {
                m_previewWorkspace = {};
                m_releasePreviewWorkspace = false;
            }
            if (!m_queue.empty())
            {
                id = m_queue.front();
                m_queue.pop_front();
                auto record = m_jobs.find(id);
                if (record == m_jobs.end() || record->second->snapshot.state != JobState::Queued)
                {
                    continue;
                }
                // 取出队列与置 Running 必须在同一把锁里，取消不能在两者之间成功。
                record->second->snapshot.state = JobState::Running;
                record->second->snapshot.startedAt = std::chrono::system_clock::now();
            }
            else if (m_pendingPreview)
            {
                preview = std::move(m_pendingPreview);
                m_runningPreview = preview;
                preview->snapshot.state = JobState::Running;
            }
        }
        if (preview)
        {
            runPreview(preview);
            continue;
        }
        if (id.empty())
        {
            continue;
        }

        std::string type;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_jobs.find(id);
            if (it != m_jobs.end()) type = it->second->snapshot.type;
        }

        if (type == "render" || type == "project-check" || type == "project-pack")
        {
            runProjectJob(id);
        }
        else if (type == "build")
        {
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
    std::shared_ptr<RuntimeContext> runtime;
    std::shared_ptr<const MosaicSelection> selection;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_jobs.find(id);
        if (it == m_jobs.end()) return;
        auto& record = it->second;
        if (record->snapshot.state != JobState::Running)
        {
            return;
        }
        request = record->request;
        runtime = record->runtime;
        selection = std::move(record->selection);
    }

    ServiceResult result;
    try
    {
        TaskResourceMonitor monitor(*runtime, request.resourceMonitor);
        MosaicService service;
        result = service.run(request, *runtime, selection.get());
    }
    catch (const std::exception& e)
    {
        result = ServiceResult::failure(1, e.what());
    }
    catch (...)
    {
        result = ServiceResult::failure(1, "internal error");
    }

    finishJob(id, runtimeResult(std::move(result), *runtime));
}

void JobManager::runBuildJob(const std::string& id)
{
    BuildRequest request;
    std::shared_ptr<RuntimeContext> runtime;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_jobs.find(id);
        if (it == m_jobs.end()) return;
        auto& record = it->second;
        if (record->snapshot.state != JobState::Running)
        {
            return;
        }
        request = record->buildRequest;
        runtime = record->runtime;
    }

    ServiceResult result;
    try
    {
        TaskResourceMonitor monitor(*runtime, request.resourceMonitor);
        BuildService service;
        result = service.run(request, *runtime);
    }
    catch (const std::exception& e)
    {
        result = ServiceResult::failure(1, e.what());
    }
    catch (...)
    {
        result = ServiceResult::failure(1, "internal error");
    }

    finishJob(id, runtimeResult(std::move(result), *runtime));
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
        // 在发布结束状态前落盘，任务结束后重启也能读取同一条摘要。
        if (m_history && m_history->available() && !m_history->record(m_sessionId, record->snapshot))
        {
            std::cerr << "WARNING: cannot save job history for " << id << '\n';
        }
        trimFinishedJobs();
    }
    m_cv.notify_all();
}

} // namespace mosaicraft
