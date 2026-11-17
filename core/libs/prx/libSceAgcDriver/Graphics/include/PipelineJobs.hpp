#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINEJOBS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINEJOBS_HPP

// nps5: a small prioritized job pool for pipeline builds, and the pipeline stamp. No Vulkan dependency.
//
// The pool's design (not code) follows the pre-warm queue idea in research/armsx2.md (ARMSX2 is GPL-3.0: ideas only):
// three priority classes, and a waiting thread runs the job itself when no worker has started it.
//   Now    a draw needs this pipeline right now.
//   Ahead  likely soon (the first entries of the journal, which is in first-use order).
//   Idle   everything else.
// Workers take Now, then Ahead, then Idle, each first-in first-out. Finish(key) claims a job that is still queued and runs it
// on the calling thread, waits for one a worker is running, and returns at once for a job that is done or unknown, so waiting
// for a pipeline never costs more than building it.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Graphics {

enum class JobPriority : std::uint8_t { Now = 0, Ahead = 1, Idle = 2 };

enum class JobQos : std::uint8_t { Background, Utility, UserInitiated };

enum class FinishResult : std::uint8_t {
    Unknown,   // no such job (done and forgotten, or never submitted): nothing to wait for
    RanInline, // the calling thread ran it
    Waited,    // a worker was running it; the caller waited
};

struct JobPoolCounters {
    std::uint64_t submitted = 0;
    std::uint64_t ran = 0;
    std::uint64_t ranInline = 0;
    std::uint64_t waited = 0;
    std::uint64_t promoted = 0;
    std::uint64_t failed = 0;
    std::uint64_t ranBy[3] = {0, 0, 0};  // by the priority the job had when it was taken
};

class PipelineJobPool {
public:
    // threads = 0 starts no workers: the owner (a test) drains with RunNext.
    PipelineJobPool(unsigned threads, JobQos qos, std::function<void()> onWorkerStart = {});
    ~PipelineJobPool();
    PipelineJobPool(const PipelineJobPool&) = delete;
    PipelineJobPool& operator=(const PipelineJobPool&) = delete;

    // False when a job with this key is already queued or running (it is not replaced).
    bool Submit(std::uint64_t key, JobPriority priority, std::function<void()> job);
    // Raises a queued job to Now; false when it is unknown or already started.
    bool Promote(std::uint64_t key);
    FinishResult Finish(std::uint64_t key);
    // True while the job is queued or running.
    [[nodiscard]] bool Pending(std::uint64_t key) const;
    // Takes and runs the best queued job on the calling thread; false when none is queued.
    bool RunNext();
    // Stops taking queued jobs (a running one completes); the destructor cancels and joins.
    void Cancel();
    // Blocks until nothing is queued or running.
    void Drain();
    [[nodiscard]] std::size_t Queued() const;
    [[nodiscard]] JobPoolCounters Counters() const;

private:
    enum class State : std::uint8_t { Queued, Running, Done };
    struct Job {
        std::uint64_t key = 0;
        JobPriority priority = JobPriority::Idle;
        std::function<void()> work;
        State state = State::Queued;
        bool waited = false;
    };
    void worker(JobQos qos, const std::function<void()>& onStart);
    std::shared_ptr<Job> take(std::unique_lock<std::mutex>& lock);
    void execute(const std::shared_ptr<Job>& job);

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable finished;
    std::deque<std::shared_ptr<Job>> lanes[3];
    std::unordered_map<std::uint64_t, std::shared_ptr<Job>> jobs;
    std::size_t running = 0;
    bool cancelled = false;
    JobPoolCounters counters;
    std::vector<std::thread> workers;
};

// The identity of a pipeline cache configuration. KosmicKrisp (Vulkan on Metal) and macOS are free to change under a title; the
// key list (the pipeline journal) must not be thrown away when they do, only the binary artefacts built from it.
struct PipelineStamp {
    std::string recompiler;  // ShaderDiskCache::SourceVersion (covers the recompiler source and its numeric switches)
    std::string driver;      // Vulkan driverInfo, driverVersion and pipelineCacheUUID (KosmicKrisp hashes its build into the UUID)
    std::string flags;       // hash of the driver switches that change generated code (MESA_KK_*, KK_*)
    std::string macos;
    std::string gpu;
    bool operator==(const PipelineStamp&) const = default;
};

enum class StampState : std::uint8_t { Absent, Fresh, Stale };

struct StampCheck {
    StampState state = StampState::Absent;
    std::vector<std::string> changed;  // names of the fields that differ, for the log
};

[[nodiscard]] std::string SerializeStamp(const PipelineStamp& stamp);
// Rejects anything that is not exactly what SerializeStamp writes (a torn or foreign file reads as absent).
[[nodiscard]] std::optional<PipelineStamp> ParseStamp(const std::string& text);
[[nodiscard]] StampCheck CompareStamp(const std::optional<PipelineStamp>& saved, const PipelineStamp& current);
[[nodiscard]] std::optional<PipelineStamp> ReadStampFile(const std::filesystem::path& path);
// Temporary file in the same directory, flushed, then renamed over the target.
bool WriteStampFile(const std::filesystem::path& path, const PipelineStamp& stamp);

// The stamp of this run (set by the driver once the device properties are known) and what it was compared against.
void SetCurrentPipelineStamp(const PipelineStamp& stamp, const std::filesystem::path& file, const StampCheck& check);
[[nodiscard]] bool CurrentPipelineStamp(PipelineStamp& stamp, StampCheck& check);
// Writes the current stamp to its file (the rebuild from the journal finished); false without a current stamp or on failure.
bool CommitCurrentPipelineStamp();

}

#endif
