// nps5: host-side helpers for NPS5_GPU=kyty (KytyPS5's GPU stack loaded by the ps5recomp runtime in place of this
// tree's libSceAgc / libSceAgcDriver / libSceVideoOut; ps5recomp docs/m3/kyty-seam.md).
//
// The game's event queues, pad library and process exit stay here. The runtime's seam (nps5_kyty.c) reaches these
// plain C functions through dlsym on libnps5_gpu:
//   Nps5KytyHostAddGpuEvent / DeleteGpuEvent / DeliverEvent   AGC end-of-pipe and VideoOut flip events on the game's queues
//   Nps5KytyHostPublishPad                                    pad state read by the GPU library's window
//   Nps5KytyHostStartPadScript                                NPS5_VPAD_SCRIPT without this tree's window (which normally plays it)
//   Nps5KytyHostRequestExit                                   LibcRequestExit, as this tree's own window close does
//   Nps5KytyHostAbi                                           version of these functions and size of the pad state they read
//
// These functions are called from C: no C++ exception leaves them (an input failure reported by the pad library, an
// allocation failure). A function that returns a status reports ErrorInternal instead.
//
// The event and pad-publish callbacks are a port of AstroHybrid's libAstroHybridHost (AnyPS5 patches 0021/0022 of the
// pcportastrobot project, GPL-2.0, a derivative of this tree): same filters, idents and trigger behaviour. Nothing here
// runs unless the runtime calls it: the default (AnyPS5 renderer) path is untouched.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "prx/libkernel/Equeue/Equeue.hpp"
#include "prx/libScePad/include/PadInputTypes.hpp"
#include "prx/libScePad/include/PadState.hpp"
#include "prx/libSceVideoOut/include/VirtualPadScript.hpp"

namespace {

constexpr std::uint32_t EventEopInterrupt = 1;
constexpr std::uint32_t EventFlip = 2;
constexpr std::int16_t FilterGraphicsCore = -14;
constexpr std::int16_t FilterVideoOut = -13;
constexpr std::uintptr_t VideoOutEventFlip = 0;
constexpr int ErrorBadQueue = static_cast<int>(0x80020009u);
constexpr int ErrorInternal = static_cast<int>(0x80020016u);
// Version of the functions exported here. The pad state layout below belongs to it.
constexpr std::uint32_t HostAbi = 2;

// kytyGpuApi.h KytyGpuPadState (ABI 1.2).
struct KytyPadTouch {
    std::uint16_t x;
    std::uint16_t y;
    std::uint8_t id;
    std::uint8_t active;
    std::uint8_t reserved[2];
};
struct KytyPadState {
    std::uint32_t buttons;
    std::uint8_t sticks[4];
    std::uint8_t l2;
    std::uint8_t r2;
    std::uint8_t connected;
    std::uint8_t reserved;
    KytyPadTouch touch[2];
};
static_assert(sizeof(KytyPadState) == 28);

struct Registration {
    KernelEqueue eq;
    std::uint32_t kind;
    std::int64_t ident;
};

std::mutex g_mutex;
std::vector<Registration> g_registrations;

// ---- motion sensors (ABI 1.5 publish_pad_motion) ----
// The library publishes a pad's SDL-normalised sensors (accel m/s^2 incl. gravity, gyro rad/s) separately from its
// buttons; they are merged into the next published pad state. A sample older than kMotionMaxAgeMs is dropped (the
// pad went away or stopped reporting), which gives the game its resting pose.
constexpr int kMotionPorts = 4;
constexpr std::uint64_t kMotionMaxAgeMs = 250;
struct MotionSlot {
    bool valid = false;
    std::array<float, 3> accel{};
    std::array<float, 3> gyro{};
    std::chrono::steady_clock::time_point time{};
};
std::mutex g_motionMutex;
MotionSlot g_motion[kMotionPorts];
std::atomic<std::uint64_t> g_motionSamples{0};
std::atomic<std::uint64_t> g_flips{0};

std::int16_t FilterOf(std::uint32_t kind) { return kind == EventFlip ? FilterVideoOut : FilterGraphicsCore; }
std::uintptr_t IdentOf(std::uint32_t kind, std::int64_t ident) { return kind == EventFlip ? VideoOutEventFlip : static_cast<std::uintptr_t>(ident); }

// ---- scripted pad (NPS5_VPAD_SCRIPT) ----
// The script thread owns its events (it is detached and may outlive the globals of this library at process exit).
std::mutex g_scriptMutex;
int g_scriptResult = 0;  // 0 = not started yet, 1 = running, -1 = rejected; under g_scriptMutex
std::atomic<bool> g_scriptStop{false};

void LogFailure(const char* where) noexcept {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) std::fprintf(stderr, "[nps5] kyty-host: %s failed with a C++ exception (ignored)\n", where);
}

std::uint32_t ButtonBit(int sdlButton) {
    switch (sdlButton) {
        case 0: return static_cast<std::uint32_t>(Pad::PadButton::Cross);
        case 1: return static_cast<std::uint32_t>(Pad::PadButton::Circle);
        case 2: return static_cast<std::uint32_t>(Pad::PadButton::Square);
        case 3: return static_cast<std::uint32_t>(Pad::PadButton::Triangle);
        case 6: return static_cast<std::uint32_t>(Pad::PadButton::Options);
        case 7: return static_cast<std::uint32_t>(Pad::PadButton::L3);
        case 8: return static_cast<std::uint32_t>(Pad::PadButton::R3);
        case 9: return static_cast<std::uint32_t>(Pad::PadButton::L1);
        case 10: return static_cast<std::uint32_t>(Pad::PadButton::R1);
        case 11: return static_cast<std::uint32_t>(Pad::PadButton::Up);
        case 12: return static_cast<std::uint32_t>(Pad::PadButton::Down);
        case 13: return static_cast<std::uint32_t>(Pad::PadButton::Left);
        case 14: return static_cast<std::uint32_t>(Pad::PadButton::Right);
        case 20: return static_cast<std::uint32_t>(Pad::PadButton::TouchPad);
        default: return 0;  // share / ps / capture: no bit in the pad data
    }
}

void ApplyScriptEvent(PadInputState& state, const Pad::VirtualPadEvent& event) {
    if (event.isShake) return;  // handled by RunScript (time-based motion)
    if (!event.isAxis) {
        const auto bit = ButtonBit(event.index);
        state.buttons = event.value != 0 ? (state.buttons | bit) : (state.buttons & ~bit);
        return;
    }
    if (event.index < 0 || event.index > 5) return;
    if (event.index < 4) {
        state.sticks[static_cast<std::size_t>(event.index)] = static_cast<std::uint8_t>(std::clamp((static_cast<int>(event.value) + 32768) >> 8, 0, 255));
        return;
    }
    const auto analog = static_cast<std::uint8_t>(std::clamp((static_cast<int>(event.value) + 32768) >> 8, 0, 255));
    const auto bit = static_cast<std::uint32_t>(event.index == 4 ? Pad::PadButton::L2 : Pad::PadButton::R2);
    (event.index == 4 ? state.analogButtonsL2 : state.analogButtonsR2) = analog;
    state.buttons = analog != 0 ? (state.buttons | bit) : (state.buttons & ~bit);
}

void RunScript(std::vector<Pad::VirtualPadEvent> script) try {
    const bool log = std::getenv("NPS5_PAD_LOG") != nullptr;
    PadInputState state{};
    PadPublishInputForPort_nid_postfix(0, state);  // the pad is connected from the start
    const auto start = std::chrono::steady_clock::now();
    const std::uint64_t startFrame = g_flips.load();
    std::size_t next = 0;
    auto lastPublish = start;
    // shake=<seconds>: a 10 Hz shake the way a hand shakes a DualSense: accel X +-2.5 g (and Z +-1.5 g, 90 degrees later) on top of gravity on Y,
    // gyro X/Z +-5 rad/s, through the same ScePadData motion fields real motion uses. Published every 5 ms while it lasts.
    std::chrono::steady_clock::time_point shakeEnd{};
    std::chrono::steady_clock::time_point shakeStart{};
    bool shaking = false;
    const bool motionLog = std::getenv("NPS5_PAD_LOG") != nullptr;
    std::array<std::chrono::steady_clock::time_point, 32> pressedAt{};
    // A shake still playing keeps the loop going after the last event (it would otherwise never play, or leave its last
    // accelerometer/gyro sample published for good).
    while (!g_scriptStop.load() && (next < script.size() || shaking)) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
        const std::uint64_t elapsedFrames = g_flips.load() - startFrame;
        bool changed = false;
        while (next < script.size()) {
            const auto& event = script[next];
            if ((event.byFrame ? elapsedFrames : elapsedMs) < event.when) break;
            const auto slot = static_cast<std::size_t>(event.index) % pressedAt.size();
            // A tap stays down at least 100 ms so the title samples it (a frame-counted tap can be a few ms long).
            if (event.tapRelease && !event.isAxis && now - pressedAt[slot] < std::chrono::milliseconds(100)) break;
            if (event.isShake) {
                shakeStart = now;
                shakeEnd = now + std::chrono::milliseconds(event.value);
                shaking = true;
                if (motionLog) std::fprintf(stderr, "[pad] kyty-host script: shake %d ms (t=%llums)\n", event.value, static_cast<unsigned long long>(elapsedMs));
                changed = true;
                ++next;
                continue;
            }
            if (!event.isAxis && event.value != 0) pressedAt[slot] = now;
            ApplyScriptEvent(state, event);
            if (log) std::fprintf(stderr, "[pad] kyty-host script: %s %d = %d (t=%llums frame+%llu)\n", event.isAxis ? "axis" : "button", event.index, event.value,
                static_cast<unsigned long long>(elapsedMs), static_cast<unsigned long long>(elapsedFrames));
            changed = true;
            ++next;
        }
        if (shaking) {
            if (now >= shakeEnd) {
                shaking = false;
                state.hasMotion = false;
                if (motionLog) std::fprintf(stderr, "[pad] kyty-host script: shake end\n");
                changed = true;
            } else if (now - lastPublish >= std::chrono::milliseconds(5)) {
                const double t = std::chrono::duration<double>(now - shakeStart).count();
                const double phase = 2.0 * 3.14159265358979 * 10.0 * t;
                constexpr float g = 9.80665f;
                state.hasMotion = true;
                state.accel = {static_cast<float>(2.5 * g * std::sin(phase)), g, static_cast<float>(1.5 * g * std::cos(phase))};
                state.gyro = {static_cast<float>(5.0 * std::cos(phase)), 0.0f, static_cast<float>(5.0 * std::sin(phase))};
                changed = true;
            }
        }
        if (changed || now - lastPublish > std::chrono::milliseconds(50)) {
            PadPublishInputForPort_nid_postfix(0, state);
            lastPublish = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    PadPublishInputForPort_nid_postfix(0, state);
} catch (...) {
    LogFailure("pad script");  // the script stops; the process goes on
}

}

extern "C" {

__attribute__((visibility("default"))) int Nps5KytyHostAddGpuEvent(std::int64_t equeue, std::uint32_t kind, std::int64_t ident, void* udata) try {
    const auto eq = static_cast<KernelEqueue>(equeue);
    if (equeue == 0 || !EqueuePin_nid_postfix(eq)) return ErrorBadQueue;
    if (kind != EventEopInterrupt && kind != EventFlip) return ErrorBadQueue;
    KernelEqueueEvent event{};
    event.event.ident = IdentOf(kind, ident);
    event.event.filter = FilterOf(kind);
    event.event.flags = kind == EventFlip ? EV_ADD : EV_ADD | EV_CLEAR;
    event.event.udata = udata;
    if (kind == EventFlip) {
        event.filter.triggerFunc = [](KernelEqueueEvent* e, void* data) {
            KernelEvent triggered = e->event;
            triggered.fflags = triggered.fflags < 0xfu ? triggered.fflags + 1u : triggered.fflags;
            triggered.data = static_cast<intptr_t>(reinterpret_cast<uintptr_t>(data));
            if (e->triggered) {
                e->pendingEvents.push_back(triggered);
            } else {
                e->event = triggered;
                e->triggered = true;
            }
        };
        event.filter.resetFunc = [](KernelEqueueEvent* e) {
            e->triggered = false;
            e->event.fflags = 0;
            e->event.data = 0;
        };
    } else {
        event.filter.triggerFunc = [](KernelEqueueEvent* e, void*) {
            e->event.data = e->triggered ? e->event.data + 1 : 1;
            e->triggered = true;
        };
        event.filter.resetFunc = [](KernelEqueueEvent* e) {
            e->triggered = false;
            e->event.data = 0;
        };
    }
    const int result = EqueueAddEvent_nid_postfix(eq, event);
    if (result != EQUEUE_OK) return result;
    std::lock_guard lock(g_mutex);
    if (std::none_of(g_registrations.begin(), g_registrations.end(), [&](const Registration& r) { return r.eq == eq && r.kind == kind && r.ident == ident; })) {
        g_registrations.push_back({eq, kind, ident});
    }
    return 0;
} catch (...) {
    LogFailure("AddGpuEvent");
    return ErrorInternal;
}

__attribute__((visibility("default"))) int Nps5KytyHostDeleteGpuEvent(std::int64_t equeue, std::uint32_t kind, std::int64_t ident) try {
    const auto eq = static_cast<KernelEqueue>(equeue);
    {
        std::lock_guard lock(g_mutex);
        std::erase_if(g_registrations, [&](const Registration& r) { return r.eq == eq && r.kind == kind && r.ident == ident; });
    }
    const int result = EqueueDeleteEvent_nid_postfix(eq, IdentOf(kind, ident), FilterOf(kind));
    return result == EQUEUE_OK ? 0 : result;
} catch (...) {
    LogFailure("DeleteGpuEvent");
    return ErrorInternal;
}

__attribute__((visibility("default"))) void Nps5KytyHostDeliverEvent(std::uint32_t kind, std::int64_t ident, std::uint64_t data) try {
    if (kind == EventFlip) g_flips.fetch_add(1);  // frame clock of the scripted pad
    std::vector<Registration> registrations;
    {
        std::lock_guard lock(g_mutex);
        registrations = g_registrations;
    }
    for (const auto& registration : registrations) {
        if (registration.kind != kind || (kind == EventEopInterrupt && registration.ident != ident)) continue;
        EqueueTriggerEvent_nid_postfix(registration.eq, IdentOf(kind, registration.ident), FilterOf(kind), reinterpret_cast<void*>(static_cast<uintptr_t>(data)));
    }
} catch (...) {
    LogFailure("DeliverEvent");
}

__attribute__((visibility("default"))) void Nps5KytyHostPublishPad(int port, const KytyPadState* state) try {
    if (state == nullptr) return;
    PadInputState input{};
    input.buttons = state->buttons;
    for (std::size_t i = 0; i < input.sticks.size(); ++i) input.sticks[i] = state->sticks[i];
    input.analogButtonsL2 = state->l2;
    input.analogButtonsR2 = state->r2;
    for (std::size_t i = 0; i < input.touch.size(); ++i) {
        input.touch[i].active = state->touch[i].active != 0;
        input.touch[i].x = state->touch[i].x;
        input.touch[i].y = state->touch[i].y;
        input.touch[i].id = state->touch[i].id;
    }
    if (port >= 0 && port < kMotionPorts) {
        std::lock_guard lock(g_motionMutex);
        const MotionSlot& m = g_motion[port];
        if (m.valid && std::chrono::steady_clock::now() - m.time < std::chrono::milliseconds(kMotionMaxAgeMs)) {
            input.hasMotion = true;
            input.accel = m.accel;
            input.gyro = m.gyro;
        }
    }
    PadPublishInputForPort_nid_postfix(port, input);
} catch (...) {
    LogFailure("PublishPad");
}

// motion: 6 floats, accel x y z (m/s^2), gyro x y z (rad/s), SDL axes. Non-finite samples are ignored.
__attribute__((visibility("default"))) void Nps5KytyHostPublishPadMotion(int port, const float* motion) try {
    if (motion == nullptr || port < 0 || port >= kMotionPorts) return;
    for (int i = 0; i < 6; ++i) {
        if (!std::isfinite(motion[i])) return;
    }
    std::lock_guard lock(g_motionMutex);
    MotionSlot& m = g_motion[port];
    m.valid = true;
    for (int i = 0; i < 3; ++i) {
        m.accel[static_cast<std::size_t>(i)] = motion[i];
        m.gyro[static_cast<std::size_t>(i)] = motion[3 + i];
    }
    m.time = std::chrono::steady_clock::now();
    g_motionSamples.fetch_add(1);
} catch (...) {
    LogFailure("PublishPadMotion");
}

// Version of this interface; *padStateBytes (may be null) receives the size of the pad state PublishPad reads. A
// caller built for another layout must not call PublishPad.
__attribute__((visibility("default"))) std::uint32_t Nps5KytyHostAbi(std::uint32_t* padStateBytes) {
    if (padStateBytes != nullptr) *padStateBytes = static_cast<std::uint32_t>(sizeof(KytyPadState));
    return HostAbi;
}

// Returns 1 when a script was started (the GPU library then need not publish a pad), 0 without NPS5_VPAD_SCRIPT, -1 on
// a script that cannot be read or parsed.
__attribute__((visibility("default"))) int Nps5KytyHostStartPadScript(void) try {
    const char* text = std::getenv("NPS5_VPAD_SCRIPT");
    if (text == nullptr || *text == '\0') return 0;
    // Once per process; a second caller waits for the first one's result instead of reading a half-parsed script.
    std::lock_guard lock(g_scriptMutex);
    if (g_scriptResult != 0) return g_scriptResult;
    g_scriptResult = -1;
    std::vector<Pad::VirtualPadEvent> script;
    std::string source = text;
    if (source.front() == '@') {
        std::ifstream file(source.substr(1));
        if (!file) {
            std::fprintf(stderr, "[pad] kyty-host: NPS5_VPAD_SCRIPT cannot read its file\n");
            return -1;
        }
        std::ostringstream content;
        content << file.rdbuf();
        source = content.str();
    }
    std::string error;
    if (!Pad::ParseVirtualPadScript(source, script, error)) {
        std::fprintf(stderr, "[pad] kyty-host: NPS5_VPAD_SCRIPT rejected: %s\n", error.c_str());
        return -1;
    }
    std::fprintf(stderr, "[pad] kyty-host: scripted pad on port 0 (%zu events)\n", script.size());
    std::thread(RunScript, std::move(script)).detach();
    g_scriptResult = 1;
    return 1;
} catch (...) {
    LogFailure("StartPadScript");
    return -1;
}

__attribute__((visibility("default"))) void Nps5KytyHostRequestExit(int code) try {
    g_scriptStop.store(true);
    LibcRequestExit_nid_postfix(code);
} catch (...) {
    LogFailure("RequestExit");
}

}
