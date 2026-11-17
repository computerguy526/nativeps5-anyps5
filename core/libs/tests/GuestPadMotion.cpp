// Motion in ScePadData: what a game that reads scePadReadState sees from the host pad's sensors (as published through
// the host seam: SDL axes, m/s^2 and rad/s). Covers the layout, gravity at rest for several controller types,
// orientation integration, scePadResetOrientation / scePadSetMotionSensorState, a shake, timestamps, a pad without
// sensors, two pads, reconnect, a sensor trace replay, and readers against a writer thread.
#include "SceTypes.hpp"
#include "prx/libScePad/include/Pad.hpp"
#include "prx/libScePad/include/PadState.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static int g_checks = 0;
static void Require(bool value, const char* what = "") {
    ++g_checks;
    if (!value) {
        std::fprintf(stderr, "GuestPadMotion: failed: %s\n", what);
        std::abort();
    }
}
static bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// ScePadData layout (PS5 SDK): orientation quaternion, then acceleration in g, then angular velocity in rad/s.
static_assert(offsetof(PadData, orientation_x) == 12);
static_assert(offsetof(PadData, orientation_w) == 24);
static_assert(offsetof(PadData, acceleration_x) == 28);
static_assert(offsetof(PadData, acceleration_z) == 36);
static_assert(offsetof(PadData, angular_velocity_x) == 40);
static_assert(offsetof(PadData, angular_velocity_z) == 48);
static_assert(offsetof(PadData, timestamp) == 80);
static_assert(sizeof(PadData) == 120);

static PadInputState Motion(std::array<float, 3> accel, std::array<float, 3> gyro) {
    PadInputState s{};
    s.hasMotion = true;
    s.accel = accel;
    s.gyro = gyro;
    return s;
}
static float Angle(const PadData& d) {
    const float v = std::sqrt(d.orientation_x * d.orientation_x + d.orientation_y * d.orientation_y + d.orientation_z * d.orientation_z);
    return 2.0f * std::atan2(v, std::fabs(d.orientation_w));
}
static float QuatNorm(const PadData& d) {
    return std::sqrt(d.orientation_x * d.orientation_x + d.orientation_y * d.orientation_y + d.orientation_z * d.orientation_z + d.orientation_w * d.orientation_w);
}
static void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static PadData Read(int handle) {
    PadData d{};
    Require(scePadReadState(handle, &d) == PAD_OK, "scePadReadState");
    return d;
}

// Mocks: flat and still on a table (SDL: gravity on +y) for the controller types SDL gives motion for.
struct MockPad { const char* name; std::array<float, 3> accel; std::array<float, 3> gyro; };
static const MockPad kMocks[] = {
    {"DualSense", {0.03f, 9.78f, 0.05f}, {0.002f, -0.001f, 0.0005f}},
    {"DualShock4", {-0.10f, 9.84f, 0.12f}, {-0.004f, 0.003f, 0.002f}},
    {"SwitchPro", {0.20f, 9.70f, -0.15f}, {0.010f, 0.007f, -0.012f}},
    {"JoyCon", {-0.18f, 9.90f, 0.20f}, {-0.008f, 0.009f, 0.004f}},
};

int main() {
    Require(scePadInit_nid_postfix() == PAD_OK, "init");
    const int handle = scePadOpen_nid_postfix(0x10000000, 0, 0, nullptr);
    Require(handle > 0, "open");
    Require(scePadSetMotionSensorState(handle, true) == PAD_OK, "motion on");

    // 1. Nothing published: the pad rests flat (1 g on +y), no rotation, identity orientation.
    {
        PadPublishInput_nid_postfix(PadInputState{});
        const PadData d = Read(handle);
        Require(Near(d.acceleration_y, 1.0f, 1e-4f) && d.acceleration_x == 0.0f && d.acceleration_z == 0.0f, "rest accel");
        Require(d.angular_velocity_x == 0.0f && d.angular_velocity_y == 0.0f && d.angular_velocity_z == 0.0f, "rest gyro");
        Require(d.orientation_w == 1.0f && d.orientation_x == 0.0f && d.orientation_y == 0.0f && d.orientation_z == 0.0f, "rest orientation");
    }

    // 2. Gravity at rest per controller type: +1 g on the +y axis, units are g (not m/s^2), gyro in rad/s untouched.
    for (const MockPad& m : kMocks) {
        PadPublishInput_nid_postfix(Motion(m.accel, m.gyro));
        const PadData d = Read(handle);
        Require(Near(d.acceleration_y, m.accel[1] / 9.80665f, 1e-4f), m.name);
        Require(d.acceleration_y > 0.98f && d.acceleration_y < 1.02f, "about +1 g on y");
        Require(Near(d.acceleration_x, m.accel[0] / 9.80665f, 1e-4f) && Near(d.acceleration_z, m.accel[2] / 9.80665f, 1e-4f), "x/z axes");
        Require(d.angular_velocity_x == m.gyro[0] && d.angular_velocity_y == m.gyro[1] && d.angular_velocity_z == m.gyro[2], "gyro passthrough");
        // Resting with gravity on +y: the tilt correction keeps the orientation at identity (no drift from a flat pad).
        Require(Angle(d) < 0.02f, "flat pad: orientation stays level");
    }

    // 3. Sign conventions: a pad on its right edge reads +x, an upside-down pad reads -y, pitched forward reads +z.
    PadPublishInput_nid_postfix(Motion({9.80665f, 0.0f, 0.0f}, {0, 0, 0}));
    Require(Read(handle).acceleration_x > 0.99f, "on its right edge: +x");
    PadPublishInput_nid_postfix(Motion({0.0f, -9.80665f, 0.0f}, {0, 0, 0}));
    Require(Read(handle).acceleration_y < -0.99f, "upside down: -y");
    PadPublishInput_nid_postfix(Motion({0.0f, 0.0f, 9.80665f}, {0, 0, 0}));
    Require(Read(handle).acceleration_z > 0.99f, "screen up: +z");

    // 4. Orientation integration: a constant 1 rad/s about +y for 0.5 s turns the pad by about 0.5 rad; the quaternion stays unit.
    Require(scePadSetTiltCorrectionState(handle, false) == PAD_OK, "tilt correction off");
    Require(scePadResetOrientation(handle) == PAD_OK, "reset");
    PadPublishInput_nid_postfix(Motion({0.0f, 9.80665f, 0.0f}, {0.0f, 1.0f, 0.0f}));
    {
        const auto t0 = std::chrono::steady_clock::now();
        Read(handle);
        PadData d{};
        for (int i = 0; i < 50; ++i) { Sleep(10); d = Read(handle); Require(Near(QuatNorm(d), 1.0f, 1e-4f), "unit quaternion"); }
        const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        Require(Near(Angle(d), 1.0f * elapsed, 0.06f * elapsed + 0.02f), "angle = rate x time");
        Require(std::fabs(d.orientation_y) > 0.2f && std::fabs(d.orientation_x) < 0.01f && std::fabs(d.orientation_z) < 0.01f, "rotation about y only");
        // 5. scePadResetOrientation brings it back to identity.
        Require(scePadResetOrientation(handle) == PAD_OK, "reset again");
        PadPublishInput_nid_postfix(Motion({0.0f, 9.80665f, 0.0f}, {0, 0, 0}));
        d = Read(handle);
        Require(d.orientation_w > 0.9999f && Angle(d) < 1e-3f, "orientation reset");
        Require(scePadResetOrientation(handle + 77) == PAD_ERROR_INVALID_HANDLE, "reset: bad handle");
    }

    // 6. Drift bound: a still pad with a small gyro bias (0.01 rad/s) and gravity on +y keeps its tilt within a few degrees
    // over a second with the tilt correction on, and the bias is not integrated into a runaway.
    Require(scePadSetTiltCorrectionState(handle, true) == PAD_OK, "tilt correction on");
    Require(scePadResetOrientation(handle) == PAD_OK, "reset");
    PadPublishInput_nid_postfix(Motion({0.0f, 9.80665f, 0.0f}, {0.01f, 0.0f, 0.01f}));
    {
        Read(handle);
        PadData d{};
        for (int i = 0; i < 60; ++i) { Sleep(10); d = Read(handle); }
        Require(Angle(d) < 0.03f, "bounded drift");
        Require(Near(QuatNorm(d), 1.0f, 1e-4f), "normalised after drift run");
    }

    // 7. scePadSetMotionSensorState(false): the game sees the resting pose whatever the pad does; on again: live.
    PadPublishInput_nid_postfix(Motion({4.903325f, 9.80665f, 0.0f}, {0.1f, 0.2f, -0.3f}));
    Require(scePadSetMotionSensorState(handle, true) == PAD_OK, "on");
    {
        const PadData d = Read(handle);
        Require(Near(d.acceleration_x, 0.5f, 0.01f) && d.angular_velocity_y == 0.2f, "live");
    }
    Require(scePadSetMotionSensorState(handle, false) == PAD_OK, "off");
    {
        const PadData d = Read(handle);
        Require(d.acceleration_x == 0.0f && Near(d.acceleration_y, 1.0f, 1e-4f) && d.angular_velocity_x == 0.0f && d.angular_velocity_y == 0.0f && d.angular_velocity_z == 0.0f, "off: resting pose");
        Require(d.orientation_w == 1.0f, "off: identity orientation");
    }
    Require(scePadSetMotionSensorState(handle, true) == PAD_OK, "on again");
    Require(Near(Read(handle).acceleration_x, 0.5f, 0.01f), "live again");
    Require(scePadSetMotionSensorState(handle + 99, true) == PAD_ERROR_INVALID_HANDLE, "state: bad handle");

    // 8. A shake: +-2 g along x at 3 Hz for 1 s, sampled at 250 Hz and read by the game at every sample. The game sees the
    // spikes (peak within [1.8, 2.05] g), no value is out of range, y stays near 1 g, the gyro passes through.
    {
        float peak = 0.0f, lowest = 0.0f;
        for (int i = 0; i < 250; ++i) {
            const float x = 2.0f * 9.80665f * std::sin(2.0f * 3.14159265f * 3.0f * (static_cast<float>(i) / 250.0f));
            PadPublishInput_nid_postfix(Motion({x, 9.80665f, 0.0f}, {0.0f, 1.0f, 0.0f}));
            const PadData d = Read(handle);
            peak = std::max(peak, d.acceleration_x);
            lowest = std::min(lowest, d.acceleration_x);
            Require(std::fabs(d.acceleration_x) <= 2.05f && Near(d.acceleration_y, 1.0f, 1e-4f), "shake sample in range");
            Require(d.angular_velocity_y == 1.0f, "shake gyro");
        }
        Require(peak > 1.8f && lowest < -1.8f, "the game sees both shake peaks");
    }

    // 9. Timestamps never go back, also with 1 kHz publishes and reads in a burst.
    {
        std::uint64_t last = 0;
        for (int i = 0; i < 2000; ++i) {
            PadPublishInput_nid_postfix(Motion({0.0f, 9.80665f, 0.0f}, {0.0f, 0.0f, 0.0f}));
            const PadData d = Read(handle);
            Require(d.timestamp >= last, "timestamp monotonic");
            last = d.timestamp;
        }
        Require(last > 0, "timestamp set");
    }

    // 10. A pad without sensors (hasMotion false): zeros and the resting pose, never a crash, also right after motion.
    PadPublishInput_nid_postfix(Motion({3.0f, 9.0f, 1.0f}, {2.0f, 2.0f, 2.0f}));
    Read(handle);
    PadPublishInput_nid_postfix(PadInputState{});
    for (int i = 0; i < 20; ++i) {
        const PadData d = Read(handle);
        Require(d.angular_velocity_x == 0.0f && d.angular_velocity_y == 0.0f && d.angular_velocity_z == 0.0f, "no sensors: gyro zero");
        Require(Near(d.acceleration_y, 1.0f, 1e-4f) && d.orientation_w == 1.0f, "no sensors: resting pose");
    }

    // 11. Two controllers: each port has its own sensors and orientation.
    {
        const int handle2 = scePadOpen_nid_postfix(0x10000001, 0, 0, nullptr);
        if (handle2 > 0) {
            PadPublishInputForPort_nid_postfix(0, Motion({0.0f, 9.80665f, 0.0f}, {0.0f, 0.0f, 0.5f}));
            PadPublishInputForPort_nid_postfix(1, Motion({9.80665f, 0.0f, 0.0f}, {0.25f, 0.0f, 0.0f}));
            const PadData a = Read(handle), b = Read(handle2);
            Require(a.angular_velocity_z == 0.5f && Near(a.acceleration_y, 1.0f, 1e-4f), "port 0 sensors");
            Require(b.angular_velocity_x == 0.25f && Near(b.acceleration_x, 1.0f, 1e-4f), "port 1 sensors");
            PadPublishInputForPort_nid_postfix(1, PadInputState{});
            Require(Read(handle2).angular_velocity_x == 0.0f && Read(handle).angular_velocity_z == 0.5f, "port 1 without sensors leaves port 0 alone");
            PadPublishInputForPort_nid_postfix(0, PadInputState{});
            PadPublishInputForPort_nid_postfix(1, PadInputState{});
        }
    }

    // 12. Reconnect: sensors vanish (pad unplugged: the orientation is dropped), come back (starts from level again).
    Require(scePadSetTiltCorrectionState(handle, false) == PAD_OK, "tilt off for reconnect");
    PadPublishInput_nid_postfix(Motion({0.0f, 9.80665f, 0.0f}, {0.0f, 2.0f, 0.0f}));
    Read(handle);
    Sleep(100);
    Require(Angle(Read(handle)) > 0.1f, "turned");
    PadPublishInput_nid_postfix(PadInputState{});
    Require(Read(handle).orientation_w == 1.0f, "unplugged: level");
    PadPublishInput_nid_postfix(Motion({0.0f, 9.80665f, 0.0f}, {0.0f, 0.0f, 0.0f}));
    Require(Angle(Read(handle)) < 1e-3f, "reconnected: starts level");
    Require(scePadSetTiltCorrectionState(handle, true) == PAD_OK, "tilt on");

    // 13. Trace replay: samples recorded from SDL (ASTROHYBRID_PAD_MOTION_REC) or the synthetic one, fed at their own
    // times; the game sees finite, in-range data, monotonic time stamps and a unit quaternion throughout.
    {
        const char* path = std::getenv("NPS5_PAD_MOTION_TRACE");
#ifdef PAD_MOTION_TRACE
        if (path == nullptr) path = PAD_MOTION_TRACE;
#endif
        std::ifstream in(path != nullptr ? path : "");
        if (in) {
            std::string line;
            std::uint64_t lastTs = 0;
            std::size_t n = 0;
            float maxAccel = 0.0f;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '#') continue;
                unsigned long long t = 0;
                float v[6];
                if (std::sscanf(line.c_str(), "%llu %f %f %f %f %f %f", &t, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 7) continue;
                PadPublishInput_nid_postfix(Motion({v[0], v[1], v[2]}, {v[3], v[4], v[5]}));
                const PadData d = Read(handle);
                Require(std::isfinite(d.acceleration_x) && std::isfinite(d.acceleration_y) && std::isfinite(d.acceleration_z), "replay finite accel");
                Require(std::isfinite(d.angular_velocity_x) && std::isfinite(d.angular_velocity_y), "replay finite gyro");
                Require(Near(QuatNorm(d), 1.0f, 1e-3f), "replay unit quaternion");
                Require(d.timestamp >= lastTs, "replay timestamps");
                lastTs = d.timestamp;
                maxAccel = std::max(maxAccel, std::fabs(d.acceleration_x));
                ++n;
            }
            Require(n > 100, "replay read its samples");
            std::printf("GuestPadMotion: replayed %zu samples from %s, peak |ax| %.2f g\n", n, path, static_cast<double>(maxAccel));
        } else {
            std::printf("GuestPadMotion: no trace file (%s), replay skipped\n", path != nullptr ? path : "unset");
        }
    }

    // 14. Threads: a writer publishing sensor samples as fast as it can against readers (game threads) calling
    // scePadReadState and the orientation reset: no crash, no torn or non-finite values, unit quaternions.
    {
        std::atomic<bool> stop{false};
        std::atomic<int> bad{0};
        std::thread writer([&] {
            unsigned i = 0;
            while (!stop.load()) {
                const float x = std::sin(static_cast<float>(i++) * 0.1f) * 20.0f;
                PadPublishInput_nid_postfix(Motion({x, 9.80665f, x * 0.1f}, {x * 0.05f, 1.0f, -x * 0.05f}));
            }
        });
        std::vector<std::thread> readers;
        for (int r = 0; r < 4; ++r) {
            readers.emplace_back([&, r] {
                for (int i = 0; i < 20000; ++i) {
                    PadData d{};
                    if (scePadReadState(handle, &d) != PAD_OK) { ++bad; continue; }
                    if (!std::isfinite(d.acceleration_x) || !std::isfinite(d.angular_velocity_x) || std::fabs(QuatNorm(d) - 1.0f) > 1e-3f) ++bad;
                    if (r == 0 && i % 500 == 0) scePadResetOrientation(handle);
                    if (r == 1 && i % 700 == 0) scePadSetMotionSensorState(handle, (i / 700) % 2 == 0);
                }
            });
        }
        for (auto& t : readers) t.join();
        stop = true;
        writer.join();
        Require(bad.load() == 0, "stress: readers vs writer");
        Require(scePadSetMotionSensorState(handle, true) == PAD_OK, "motion on after the stress");
    }

    PadPublishInput_nid_postfix(PadInputState{});
    Require(scePadClose_nid_postfix(handle) == PAD_OK, "close");
    std::printf("GuestPadMotion: %d checks passed\n", g_checks);
    return 0;
}
