#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_ABSWITCH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_ABSWITCH_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

// In-run A/B switching of runtime optimisation flags, counted in frames.
//
//   APS5_AB_PERIOD=<frames>        one [ab] line per N flips; the mode alternates A, B, A, B ... every APS5_AB_MODE_PERIODS lines
//   APS5_AB_FLAGS=<name>[,<name>]  the registered flags that are toggled in mode B
//   APS5_AB_MODE_PERIODS=<n>       periods per mode visit (default 2); the first period after a
//                                  switch is warm-up (since_switch=0) and ab_summary.py drops it
//
// Mode A is the baseline (every flag as its owner configured it). In mode B each flag
// named in APS5_AB_FLAGS has the opposite value. Flags not listed never change.
// Every period one "[ab] mode=A|B ..." line is printed with the frame time p50/p90 and
// the CPU ms per frame of the draw-commit and queue worker threads in that period.
//
// Use from code:
//   static AbSwitch::Flag fast("my_fast_path", /*baseline=*/true);
//   if (fast.on()) {...}              // one relaxed atomic load plus a xor
// or, where a static handle is awkward:
//   if (APS5_AB_ENABLED("my_fast_path", true)) {...}
// Names that match an existing RuntimeSwitch (APS5_NO_BARRIER_BATCH ...) also work in
// APS5_AB_FLAGS: the switch is assigned at each mode change.
namespace AgcDriver::Graphics::AbSwitch {

// Current mode: false = A, true = B. Written only by OnFlip().
inline std::atomic<bool> modeB{false};

// True when the flag named `name` is listed in APS5_AB_FLAGS (parsed once).
inline bool Listed(std::string_view name) {
    const char* text = std::getenv("APS5_AB_FLAGS");
    std::string_view rest = text != nullptr ? text : "";
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        auto item = rest.substr(0, comma);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
        if (item == name) return true;
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
    }
    return false;
}

class Flag {
public:
    Flag(const char* name, bool baseline) : baseline(baseline), toggled(Listed(name)) {}
    bool on() const { return baseline != (toggled && modeB.load(std::memory_order_relaxed)); }
    explicit operator bool() const { return on(); }
private:
    bool baseline;
    bool toggled;
};

// Convenience lookup by name (registers the flag on first use; the lookup itself
// is cached per call site by the macro).
#define APS5_AB_ENABLED(name, baseline) ([]() -> const ::AgcDriver::Graphics::AbSwitch::Flag& { static const ::AgcDriver::Graphics::AbSwitch::Flag f(name, baseline); return f; }().on())

// True when APS5_AB_PERIOD is set.
bool Configured();

// Call once per flip. Cheap (an inline check) when A/B is not configured.
void OnFlip();

// Remember the calling thread for the per-mode CPU figures. kind: "commit" or "qworker".
void RegisterThread(const char* kind);

// Parsing and statistics, exposed for tests.
std::uint64_t ParsePeriod(const char* text);
double Percentile(double* values, std::size_t count, double fraction);

}

#endif
