#pragma once

#include "BuildService.h"
#include "MosaicPlanning.h"
#include "MosaicSelection.h"
#include "MosaicService.h"
#include "PreviewEvaluation.h"
#include "ProjectService.h"

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
class HistoryStore;

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

struct PreviewSnapshot
{
    std::string id;
    JobState state = JobState::Queued;
    ServiceResult result;
    ProgressSnapshot progress;
    std::shared_ptr<const MosaicSelection> selection;
    std::optional<FileSizeSample> fileSample;
    std::shared_ptr<const PreviewComparison> comparison;
    MosaicRequest request;
    uint32_t seed = 0;
};

class JobManager
{
public:
    explicit JobManager(bool autoStartWorker = true, bool persistentHistory = false);
    ~JobManager();

    JobManager(const JobManager&) = delete;
    JobManager& operator=(const JobManager&) = delete;

    std::string submitMosaic(MosaicRequest request, std::shared_ptr<const MosaicSelection> selection = {});
    std::string submitBuild(BuildRequest request);
    std::string submitProject(const std::string &type, ProjectRequest request);
    std::string submitPreview(MosaicRequest request, uint32_t seed,
                              std::shared_ptr<const MosaicSelection> source = {},
                              std::shared_ptr<const MosaicSelection> compareRight = {});
    bool getPreview(const std::string &id, PreviewSnapshot &out);
    bool closePreview(const std::string &id);
    bool pinPreview(int slot, const std::string &id);
    HistoryStore *history() const { return m_history.get(); }
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
        ProjectRequest projectRequest;
        std::shared_ptr<const MosaicSelection> selection;
    };

    struct PreviewJob
    {
        PreviewSnapshot snapshot;
        std::shared_ptr<const MosaicSelection> source, compareRight;
        std::shared_ptr<RuntimeContext> runtime = std::make_shared<RuntimeContext>(false);
        std::atomic_bool canceled{false};
        bool interrupted = false;
        std::chrono::steady_clock::time_point queuedAt, accessed;
    };

    std::string nextId();
    void trimFinishedJobs();
    void workerLoop();
    void runMosaicJob(const std::string& id);
    void runBuildJob(const std::string& id);
    void runProjectJob(const std::string &id);
    void runPreview(const std::shared_ptr<PreviewJob> &job);
    void interruptPreview();
    void trimPreviews();
    void finishJob(const std::string& id, const ServiceResult& result);

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::unordered_map<std::string, std::shared_ptr<JobRecord>> m_jobs;
    std::deque<std::string> m_queue;
    std::unordered_map<std::string, std::shared_ptr<PreviewJob>> m_previews;
    std::array<std::string, 2> m_pinnedPreviews;
    std::shared_ptr<PreviewJob> m_pendingPreview, m_runningPreview;
    PreviewWorkspace m_previewWorkspace;
    bool m_releasePreviewWorkspace = false;
    bool m_stopping = false;
    std::unique_ptr<HistoryStore> m_history;
    std::thread m_worker;
    uint64_t m_nextId = 1;
    uint64_t m_nextPreviewId = 1;
    std::string m_sessionId;
};

const char* jobStateName(JobState state);

} // namespace mosaicraft
