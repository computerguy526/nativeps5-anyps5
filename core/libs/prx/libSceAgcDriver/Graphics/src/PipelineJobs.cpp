#include "prx/libSceAgcDriver/Graphics/include/PipelineJobs.hpp"
#include "prx/libc/include/HostThread.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif
#include <unistd.h>

namespace AgcDriver::Graphics {

PipelineJobPool::PipelineJobPool(unsigned threads, JobQos qos, std::function<void()> onWorkerStart) {
    workers.reserve(threads);
    for (unsigned index = 0; index < threads; ++index) workers.push_back(Libc::StartHostThread([this, qos, onWorkerStart] { worker(qos, onWorkerStart); }));
}

PipelineJobPool::~PipelineJobPool() {
    Cancel();
    for (auto& thread : workers) {
        if (thread.joinable()) thread.join();
    }
}

bool PipelineJobPool::Submit(std::uint64_t key, JobPriority priority, std::function<void()> work) {
    {
        std::lock_guard lock(mutex);
        if (cancelled || jobs.contains(key)) return false;
        auto job = std::make_shared<Job>();
        job->key = key;
        job->priority = priority;
        job->work = std::move(work);
        jobs.emplace(key, job);
        lanes[static_cast<std::size_t>(priority)].push_back(std::move(job));
        ++counters.submitted;
    }
    wake.notify_one();
    return true;
}

bool PipelineJobPool::Promote(std::uint64_t key) {
    {
        std::lock_guard lock(mutex);
        const auto found = jobs.find(key);
        if (found == jobs.end() || found->second->state != State::Queued) return false;
        auto& job = found->second;
        if (job->priority == JobPriority::Now) return true;
        // Lanes hold lazily: the old entry is skipped when its priority no longer matches the lane it sits in.
        job->priority = JobPriority::Now;
        lanes[0].push_back(job);
        ++counters.promoted;
    }
    wake.notify_one();
    return true;
}

std::shared_ptr<PipelineJobPool::Job> PipelineJobPool::take(std::unique_lock<std::mutex>& lock) {
    static_cast<void>(lock);
    for (std::size_t lane = 0; lane < 3; ++lane) {
        auto& queue = lanes[lane];
        while (!queue.empty()) {
            auto job = std::move(queue.front());
            queue.pop_front();
            // Stale entries: claimed by Finish, or promoted out of this lane.
            if (job->state != State::Queued || static_cast<std::size_t>(job->priority) != lane) continue;
            job->state = State::Running;
            ++running;
            ++counters.ranBy[lane];
            return job;
        }
    }
    return nullptr;
}

void PipelineJobPool::execute(const std::shared_ptr<Job>& job) {
    bool failed = false;
    try {
        job->work();
    } catch (...) {
        failed = true;
    }
    {
        std::lock_guard lock(mutex);
        job->state = State::Done;
        jobs.erase(job->key);
        --running;
        ++counters.ran;
        if (failed) ++counters.failed;
    }
    finished.notify_all();
}

FinishResult PipelineJobPool::Finish(std::uint64_t key) {
    std::shared_ptr<Job> job;
    {
        std::unique_lock lock(mutex);
        const auto found = jobs.find(key);
        if (found == jobs.end()) return FinishResult::Unknown;
        job = found->second;
        if (job->state == State::Queued) {
            job->state = State::Running;
            ++running;
            ++counters.ranBy[static_cast<std::size_t>(job->priority)];
            ++counters.ranInline;
            lock.unlock();
            execute(job);
            return FinishResult::RanInline;
        }
        ++counters.waited;
        finished.wait(lock, [&] { return job->state == State::Done; });
        return FinishResult::Waited;
    }
}

bool PipelineJobPool::Pending(std::uint64_t key) const {
    std::lock_guard lock(mutex);
    return jobs.contains(key);
}

bool PipelineJobPool::RunNext() {
    std::shared_ptr<Job> job;
    {
        std::unique_lock lock(mutex);
        if (cancelled) return false;
        job = take(lock);
    }
    if (job == nullptr) return false;
    execute(job);
    return true;
}

void PipelineJobPool::Cancel() {
    {
        std::lock_guard lock(mutex);
        cancelled = true;
        // Queued jobs are dropped; a Finish waiting on one would never return, so they complete as skipped.
        for (auto& lane : lanes) lane.clear();
        for (auto it = jobs.begin(); it != jobs.end();) {
            if (it->second->state == State::Queued) {
                it->second->state = State::Done;
                it = jobs.erase(it);
            } else {
                ++it;
            }
        }
    }
    wake.notify_all();
    finished.notify_all();
}

void PipelineJobPool::Drain() {
    std::unique_lock lock(mutex);
    finished.wait(lock, [&] { return jobs.empty() && running == 0; });
}

std::size_t PipelineJobPool::Queued() const {
    std::lock_guard lock(mutex);
    return static_cast<std::size_t>(std::count_if(jobs.begin(), jobs.end(), [](const auto& entry) { return entry.second->state == State::Queued; }));
}

JobPoolCounters PipelineJobPool::Counters() const {
    std::lock_guard lock(mutex);
    return counters;
}

void PipelineJobPool::worker(JobQos qos, const std::function<void()>& onStart) {
#ifdef __APPLE__
    // Utility and background classes run on the efficiency cores unless the performance cores are idle, so the title's own
    // threads and WindowServer keep the fast cores.
    const auto klass = qos == JobQos::Background ? QOS_CLASS_BACKGROUND : qos == JobQos::Utility ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INITIATED;
    pthread_set_qos_class_self_np(klass, 0);
#else
    static_cast<void>(qos);
#endif
    if (onStart) onStart();
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(mutex);
            for (;;) {
                if (cancelled) return;
                job = take(lock);
                if (job != nullptr) break;
                wake.wait(lock);
            }
        }
        execute(job);
    }
}

namespace {

constexpr const char* StampMagic = "nps5-pipeline-stamp 1";

struct CurrentStamp {
    std::mutex mutex;
    bool set = false;
    PipelineStamp stamp;
    std::filesystem::path file;
    StampCheck check;
};

CurrentStamp& current() {
    static auto* value = new CurrentStamp;
    return *value;
}

bool cleanField(const std::string& value) {
    return value.find('\n') == std::string::npos && value.find('\r') == std::string::npos;
}

}

std::string SerializeStamp(const PipelineStamp& stamp) {
    std::ostringstream out;
    out << StampMagic << '\n'
        << "recompiler=" << stamp.recompiler << '\n'
        << "driver=" << stamp.driver << '\n'
        << "flags=" << stamp.flags << '\n'
        << "macos=" << stamp.macos << '\n'
        << "gpu=" << stamp.gpu << '\n'
        << "end\n";
    return out.str();
}

std::optional<PipelineStamp> ParseStamp(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != StampMagic) return std::nullopt;
    PipelineStamp stamp;
    const char* names[] = {"recompiler=", "driver=", "flags=", "macos=", "gpu="};
    std::string* fields[] = {&stamp.recompiler, &stamp.driver, &stamp.flags, &stamp.macos, &stamp.gpu};
    for (std::size_t index = 0; index < 5; ++index) {
        if (!std::getline(in, line) || line.rfind(names[index], 0) != 0) return std::nullopt;
        *fields[index] = line.substr(std::strlen(names[index]));
    }
    if (!std::getline(in, line) || line != "end") return std::nullopt;  // a torn write has no end marker
    return stamp;
}

StampCheck CompareStamp(const std::optional<PipelineStamp>& saved, const PipelineStamp& now) {
    StampCheck check;
    if (!saved) return check;
    check.state = StampState::Fresh;
    const auto note = [&](const char* name, const std::string& before, const std::string& after) {
        if (before != after) check.changed.emplace_back(name);
    };
    note("recompiler", saved->recompiler, now.recompiler);
    note("driver", saved->driver, now.driver);
    note("flags", saved->flags, now.flags);
    note("macos", saved->macos, now.macos);
    note("gpu", saved->gpu, now.gpu);
    if (!check.changed.empty()) check.state = StampState::Stale;
    return check;
}

std::optional<PipelineStamp> ReadStampFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (text.size() > 4096) return std::nullopt;
    return ParseStamp(text);
}

bool WriteStampFile(const std::filesystem::path& path, const PipelineStamp& stamp) {
    if (!cleanField(stamp.recompiler) || !cleanField(stamp.driver) || !cleanField(stamp.flags) || !cleanField(stamp.macos) || !cleanField(stamp.gpu)) return false;
    const auto text = SerializeStamp(stamp);
    auto temporary = path;
    temporary += ".tmp." + std::to_string(::getpid());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        if (!file) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

void SetCurrentPipelineStamp(const PipelineStamp& stamp, const std::filesystem::path& file, const StampCheck& check) {
    auto& value = current();
    std::lock_guard lock(value.mutex);
    value.set = true;
    value.stamp = stamp;
    value.file = file;
    value.check = check;
}

bool CurrentPipelineStamp(PipelineStamp& stamp, StampCheck& check) {
    auto& value = current();
    std::lock_guard lock(value.mutex);
    if (!value.set) return false;
    stamp = value.stamp;
    check = value.check;
    return true;
}

bool CommitCurrentPipelineStamp() {
    auto& value = current();
    std::lock_guard lock(value.mutex);
    if (!value.set || value.file.empty()) return false;
    if (!WriteStampFile(value.file, value.stamp)) return false;
    value.check = StampCheck{StampState::Fresh, {}};
    return true;
}

}
