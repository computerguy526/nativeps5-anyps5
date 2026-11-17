// nps5: unit tests of the prioritized pipeline job pool and the pipeline stamp (Graphics/src/PipelineJobs.cpp). No Vulkan.
#include "prx/libSceAgcDriver/Graphics/include/PipelineJobs.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace AgcDriver::Graphics;
using namespace std::chrono_literals;

namespace {

int failures = 0;

void Check(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

// Priority ordering: with no workers the test takes jobs in the order a worker would.
void PriorityOrdering() {
    PipelineJobPool pool(0, JobQos::Utility);
    std::vector<int> order;
    const auto record = [&](int id) { return [&order, id] { order.push_back(id); }; };
    Check(pool.Submit(1, JobPriority::Idle, record(1)), "submit idle 1");
    Check(pool.Submit(2, JobPriority::Ahead, record(2)), "submit ahead 2");
    Check(pool.Submit(3, JobPriority::Idle, record(3)), "submit idle 3");
    Check(pool.Submit(4, JobPriority::Now, record(4)), "submit now 4");
    Check(pool.Submit(5, JobPriority::Ahead, record(5)), "submit ahead 5");
    Check(pool.Submit(6, JobPriority::Now, record(6)), "submit now 6");
    Check(!pool.Submit(4, JobPriority::Now, record(40)), "a second job with the same key is refused");
    Check(pool.Queued() == 6, "six queued");
    Check(pool.Promote(3), "promote queued idle job 3");
    Check(!pool.Promote(99), "promote of an unknown key fails");
    while (pool.RunNext()) {}
    // Now first (FIFO: 4, 6, then 3 which was promoted last), then Ahead (2, 5), then Idle (1).
    const std::vector<int> expected{4, 6, 3, 2, 5, 1};
    Check(order == expected, "order is Now (4,6,3 promoted), Ahead (2,5), Idle (1)");
    const auto counters = pool.Counters();
    Check(counters.ranBy[0] == 3 && counters.ranBy[1] == 2 && counters.ranBy[2] == 1, "ran counts per priority at the time of taking");
    Check(counters.promoted == 1, "one promotion");
    Check(!pool.Pending(4), "done jobs are forgotten");
}

// A waiting thread runs a job nobody has started.
void WaiterRunsTheJob() {
    PipelineJobPool pool(0, JobQos::Utility);  // no worker will ever start it
    std::thread::id ranOn;
    std::atomic<int> runs{0};
    pool.Submit(7, JobPriority::Idle, [&] {
        ranOn = std::this_thread::get_id();
        ++runs;
    });
    Check(pool.Pending(7), "pending before Finish");
    Check(pool.Finish(7) == FinishResult::RanInline, "Finish runs a queued job itself");
    Check(ranOn == std::this_thread::get_id(), "on the calling thread");
    Check(runs == 1 && !pool.Pending(7), "ran once and is done");
    Check(pool.Finish(7) == FinishResult::Unknown, "Finish of a done job returns at once");
    Check(pool.RunNext() == false, "the claimed job is not run a second time by a worker");
    Check(pool.Counters().ranInline == 1, "counted as inline");
}

// With every worker busy on a long job, a Now waiter still gets its queued job through.
void WaiterBeatsBusyWorkers() {
    PipelineJobPool pool(1, JobQos::Utility);
    std::promise<void> release;
    std::promise<void> started;
    auto released = release.get_future().share();
    pool.Submit(1, JobPriority::Now, [&] {
        started.set_value();
        released.wait();
    });
    started.get_future().wait();  // the only worker is now blocked
    std::atomic<bool> ran{false};
    pool.Submit(2, JobPriority::Idle, [&] { ran = true; });
    const auto begin = std::chrono::steady_clock::now();
    Check(pool.Finish(2) == FinishResult::RanInline, "a queued job behind a blocked worker is run by the waiter");
    Check(ran.load(), "the job ran");
    Check(std::chrono::steady_clock::now() - begin < 5s, "without waiting for the blocked worker");
    release.set_value();
    pool.Drain();
}

// A job a worker is already running is waited for, not run twice.
void WaiterWaitsForRunningJob() {
    PipelineJobPool pool(1, JobQos::Utility);
    std::promise<void> release;
    std::promise<void> started;
    auto released = release.get_future().share();
    std::atomic<int> runs{0};
    pool.Submit(1, JobPriority::Now, [&] {
        ++runs;
        started.set_value();
        released.wait();
    });
    started.get_future().wait();
    std::atomic<bool> finished{false};
    std::thread waiter([&] {
        Check(pool.Finish(1) == FinishResult::Waited, "Finish waits for a running job");
        finished = true;
    });
    std::this_thread::sleep_for(100ms);
    Check(!finished.load(), "the waiter is still waiting while the job runs");
    release.set_value();
    waiter.join();
    Check(finished.load() && runs == 1, "released, and the job ran exactly once");
}

void ExceptionsAreContained() {
    PipelineJobPool pool(0, JobQos::Utility);
    pool.Submit(1, JobPriority::Now, [] { throw 1; });
    Check(pool.RunNext(), "the failing job ran");
    Check(pool.Counters().failed == 1 && !pool.Pending(1), "failure counted, job gone");
}

void CancelDropsQueuedJobs() {
    PipelineJobPool pool(0, JobQos::Utility);
    std::atomic<int> runs{0};
    pool.Submit(1, JobPriority::Idle, [&] { ++runs; });
    pool.Cancel();
    Check(!pool.RunNext() && runs == 0, "nothing runs after Cancel");
    Check(pool.Finish(1) == FinishResult::Unknown, "Finish of a cancelled job returns");
    Check(!pool.Submit(2, JobPriority::Now, [] {}), "no new jobs after Cancel");
}

void ManyWorkers() {
    PipelineJobPool pool(4, JobQos::Utility);
    std::atomic<int> sum{0};
    for (int index = 0; index < 500; ++index) pool.Submit(1000 + index, index % 3 == 0 ? JobPriority::Now : index % 3 == 1 ? JobPriority::Ahead : JobPriority::Idle, [&sum] { ++sum; });
    pool.Drain();
    Check(sum == 500, "all 500 jobs ran on 4 workers");
}

PipelineStamp BaseStamp() {
    return {"28f886242a4c8024", "KosmicKrisp Mesa 26.x|abcd|123", "none", "25G83", "Apple M4 Pro|106b:0000"};
}

void StampSurvivesDriverChange() {
    const auto saved = BaseStamp();
    Check(ParseStamp(SerializeStamp(saved)) == saved, "stamp round trip");
    Check(CompareStamp(saved, BaseStamp()).state == StampState::Fresh, "same stamp is fresh");
    Check(CompareStamp(std::nullopt, BaseStamp()).state == StampState::Absent, "no file is absent");
    auto kk = BaseStamp();
    kk.driver = "KosmicKrisp Mesa 26.x|ffff|124";
    const auto driver = CompareStamp(saved, kk);
    Check(driver.state == StampState::Stale && driver.changed == std::vector<std::string>{"driver"}, "a KosmicKrisp change is stale and names the driver only");
    auto flags = BaseStamp();
    flags.flags = "0123456789abcdef";
    Check(CompareStamp(saved, flags).changed == std::vector<std::string>{"flags"}, "a driver flag change is stale");
    auto many = BaseStamp();
    many.macos = "25G84";
    many.recompiler = "00000000deadbeef";
    Check(CompareStamp(saved, many).changed == (std::vector<std::string>{"recompiler", "macos"}), "several fields are all named");
}

void StampFileRejectsDamage() {
    const auto dir = std::filesystem::temp_directory_path() / ("nps5_stamp_test_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto file = dir / "pipeline.stamp";
    Check(!ReadStampFile(file).has_value(), "a missing file reads as absent");
    Check(WriteStampFile(file, BaseStamp()), "write");
    Check(ReadStampFile(file) == BaseStamp(), "read back");
    std::size_t leftovers = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) leftovers += entry.path().filename() != "pipeline.stamp";
    Check(leftovers == 0, "the temporary file was renamed away");
    const auto text = SerializeStamp(BaseStamp());
    {
        std::ofstream torn(file, std::ios::binary | std::ios::trunc);
        torn.write(text.data(), static_cast<std::streamsize>(text.size() - 6));  // cut off before "end"
    }
    Check(!ReadStampFile(file).has_value(), "a torn stamp (no end marker) is rejected");
    {
        std::ofstream junk(file, std::ios::binary | std::ios::trunc);
        junk << "not a stamp\n";
    }
    Check(!ReadStampFile(file).has_value(), "a foreign file is rejected");
    auto bad = BaseStamp();
    bad.gpu = "line\nbreak";
    Check(!WriteStampFile(file, bad), "a field with a newline is refused");
    std::filesystem::remove_all(dir);
}

}

int main() {
    PriorityOrdering();
    WaiterRunsTheJob();
    WaiterBeatsBusyWorkers();
    WaiterWaitsForRunningJob();
    ExceptionsAreContained();
    CancelDropsQueuedJobs();
    ManyWorkers();
    StampSurvivesDriverChange();
    StampFileRejectsDamage();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("pipeline_jobs_tests: OK");
    return 0;
}
