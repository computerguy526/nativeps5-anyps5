// nps5: unit test of the C boundary of core/libs/nps5/KytyHost.cpp with stand-ins for the libraries behind it (no
// window, no real queues): no C++ exception leaves an exported function, the pad script is started once and owned
// by its thread, and the interface version / pad state size are reported. Target kyty_host_unit_tests.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#include "SceTypes.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "prx/libkernel/Equeue/Equeue.hpp"
#include "prx/libScePad/include/PadInputTypes.hpp"
#include "prx/libScePad/include/PadState.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static std::atomic<bool> g_throw{false};
static std::atomic<int> g_published{0}, g_added{0}, g_triggered{0}, g_exit{-1};
static void maybeThrow() { if (g_throw.load()) throw std::runtime_error("stand-in failure"); }

extern "C" {
KernelEqueueRef EqueuePin_nid_postfix(KernelEqueue eq) {
    maybeThrow();
    // Non-null for queue 5 only; the object is never touched by the code under test.
    return eq == 5 ? KernelEqueueRef(std::shared_ptr<void>(), reinterpret_cast<KernelEqueuePrivate*>(std::uintptr_t{0x1000})) : nullptr;
}
int APS5_VABI EqueueAddEvent_nid_postfix(KernelEqueue, const KernelEqueueEvent&) { ++g_added; return EQUEUE_OK; }
int APS5_VABI EqueueTriggerEvent_nid_postfix(KernelEqueue, uintptr_t, int16_t, void*) { maybeThrow(); ++g_triggered; return EQUEUE_OK; }
int APS5_VABI EqueueDeleteEvent_nid_postfix(KernelEqueue, uintptr_t, int16_t) { maybeThrow(); return EQUEUE_OK; }
void PadPublishInputForPort_nid_postfix(int, const PadInputState&) { maybeThrow(); ++g_published; }
void LibcRequestExit_nid_postfix(int code) { maybeThrow(); g_exit = code; }

int Nps5KytyHostAddGpuEvent(std::int64_t equeue, std::uint32_t kind, std::int64_t ident, void* udata);
int Nps5KytyHostDeleteGpuEvent(std::int64_t equeue, std::uint32_t kind, std::int64_t ident);
void Nps5KytyHostDeliverEvent(std::uint32_t kind, std::int64_t ident, std::uint64_t data);
void Nps5KytyHostPublishPad(int port, const void* state);
std::uint32_t Nps5KytyHostAbi(std::uint32_t* padStateBytes);
int Nps5KytyHostStartPadScript(void);
void Nps5KytyHostRequestExit(int code);
}

int main() {
    const int errorBadQueue = static_cast<int>(0x80020009u), errorInternal = static_cast<int>(0x80020016u);
    std::uint32_t padBytes = 0;
    CHECK(Nps5KytyHostAbi(&padBytes) == 2 && padBytes == 28);
    CHECK(Nps5KytyHostAbi(nullptr) == 2);

    // Arguments from the other side.
    CHECK(Nps5KytyHostAddGpuEvent(0, 1, 7, nullptr) == errorBadQueue);
    CHECK(Nps5KytyHostAddGpuEvent(9, 1, 7, nullptr) == errorBadQueue);   // not a queue
    CHECK(Nps5KytyHostAddGpuEvent(5, 99, 7, nullptr) == errorBadQueue);  // not an event kind
    CHECK(Nps5KytyHostAddGpuEvent(5, 1, 7, nullptr) == 0 && g_added == 1);
    CHECK(Nps5KytyHostAddGpuEvent(5, 2, 0, nullptr) == 0);
    Nps5KytyHostDeliverEvent(1, 7, 0);
    Nps5KytyHostDeliverEvent(1, 8, 0);  // other ident: nobody
    Nps5KytyHostDeliverEvent(2, 0, 0);
    CHECK(g_triggered == 2);
    Nps5KytyHostPublishPad(0, nullptr);
    CHECK(g_published == 0);
    unsigned char pad[28] = {};
    Nps5KytyHostPublishPad(0, pad);
    CHECK(g_published == 1);

    // A failure behind the boundary stays behind it.
    g_throw = true;
    CHECK(Nps5KytyHostAddGpuEvent(5, 1, 9, nullptr) == errorInternal);
    CHECK(Nps5KytyHostDeleteGpuEvent(5, 1, 7) == errorInternal);
    Nps5KytyHostDeliverEvent(2, 0, 0);
    Nps5KytyHostPublishPad(0, pad);
    Nps5KytyHostRequestExit(3);
    CHECK(g_exit == -1 && g_published == 1);
    g_throw = false;

    // Pad script: nothing without the variable, a rejected script stays rejected, a good one starts once for
    // every concurrent caller (separate processes: the result is fixed for the life of a process).
    unsetenv("NPS5_VPAD_SCRIPT");
    CHECK(Nps5KytyHostStartPadScript() == 0);
    std::fflush(nullptr);
    pid_t child = fork();
    if (child == 0) {
        setenv("NPS5_VPAD_SCRIPT", "this is not a script", 1);
        const bool ok = Nps5KytyHostStartPadScript() == -1 && Nps5KytyHostStartPadScript() == -1;
        _exit(ok ? 0 : 1);
    }
    int status = -1;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    child = fork();
    if (child == 0) {
        setenv("NPS5_VPAD_SCRIPT", "20:+cross;2000:-cross", 1);
        std::atomic<int> bad{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) threads.emplace_back([&] { if (Nps5KytyHostStartPadScript() != 1) ++bad; });
        for (auto& t : threads) t.join();
        usleep(150 * 1000);
        const int published = g_published.load();
        // The script thread meets a failing pad library: the process goes on.
        g_throw = true;
        usleep(150 * 1000);
        g_throw = false;
        Nps5KytyHostRequestExit(4);
        _exit(bad == 0 && published >= 2 && g_exit == 4 ? 0 : 1);
    }
    status = -1;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    if (failures == 0) std::puts("kyty host unit tests passed");
    return failures == 0 ? 0 : 1;
}
