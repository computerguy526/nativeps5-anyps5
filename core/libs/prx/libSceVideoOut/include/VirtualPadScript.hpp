#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_VIRTUALPADSCRIPT_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_VIRTUALPADSCRIPT_HPP

// nps5: a scripted SDL virtual joystick (NPS5_VPAD_SCRIPT), so pad input can be driven without hardware.
//
// NPS5_VPAD_SCRIPT is a list of events separated by ';' or newlines (or "@path" to read them from a file):
//     <when>:<op>
//   <when>  milliseconds since the first PadInput::Update ("1500"), or a presented-frame number ("f120")
//   <op>    +<button>        press
//           -<button>        release
//           tap=<button>     press now, release 120 ms (or 4 frames, for "f") later
//           shake=<seconds>  shake the controller (accelerometer + gyro, about 2.5 g at 10 Hz) for that long (at most 30 s); plays in the Kyty host only
//           <axis>=<value>   lx ly rx ry in -1..1 (down and right are positive), l2 r2 in 0..1 (the pad starts with both triggers released)
// Buttons use SDL's positional names: a/south/cross, b/east/circle, x/west/square, y/north/triangle, back/share/view,
// guide/ps/home, start/options, l3, r3, l1, r1, up, down, left, right, misc1/capture, touchpad.
// NPS5_VPAD_TYPE selects the identity the virtual pad reports: ps5, switch (default is a generic pad), xbox.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace Pad {

struct VirtualPadEvent {
    bool byFrame = false;
    std::uint64_t when = 0;
    bool isAxis = false;
    int index = 0;           // SDL_GameControllerButton or SDL_GameControllerAxis
    std::int16_t value = 0;  // button: 0/1, axis: -32768..32767
    bool isShake = false;    // shake=<seconds>: a controller shake (motion sensors), value = duration in ms (millisecond clock only)
    bool tapRelease = false; // release of a tap: held at least 4 flips (f) / 120 ms (ms) AND 100 ms AND 2 guest pad reads after the press
};

namespace detail {

inline std::string lowerCopy(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

inline std::string_view trimView(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.remove_suffix(1);
    return text;
}

// SDL_GameControllerButton values, repeated here so the parser needs no SDL header: A=0 B=1 X=2 Y=3 BACK=4 GUIDE=5
// START=6 LS=7 RS=8 LB=9 RB=10 UP=11 DOWN=12 LEFT=13 RIGHT=14 MISC1=15 PADDLE1..4=16..19 TOUCHPAD=20.
inline int buttonIndex(std::string_view name) {
    struct Entry { const char* name; int index; };
    static constexpr Entry table[] = {
        {"a", 0}, {"south", 0}, {"cross", 0}, {"b", 1}, {"east", 1}, {"circle", 1}, {"x", 2}, {"west", 2}, {"square", 2},
        {"y", 3}, {"north", 3}, {"triangle", 3}, {"back", 4}, {"share", 4}, {"view", 4}, {"minus", 4}, {"guide", 5},
        {"ps", 5}, {"home", 5}, {"start", 6}, {"options", 6}, {"plus", 6}, {"l3", 7}, {"r3", 8}, {"l1", 9}, {"lb", 9},
        {"r1", 10}, {"rb", 10}, {"up", 11}, {"down", 12}, {"left", 13}, {"right", 14}, {"misc1", 15}, {"capture", 15},
        {"touchpad", 20},
    };
    const auto lower = lowerCopy(name);
    for (const auto& entry : table) {
        if (lower == entry.name) return entry.index;
    }
    return -1;
}

// SDL_GameControllerAxis: LEFTX=0 LEFTY=1 RIGHTX=2 RIGHTY=3 TRIGGERLEFT=4 TRIGGERRIGHT=5.
inline int axisIndex(std::string_view name) {
    const auto lower = lowerCopy(name);
    if (lower == "lx") return 0;
    if (lower == "ly") return 1;
    if (lower == "rx") return 2;
    if (lower == "ry") return 3;
    if (lower == "l2" || lower == "lt") return 4;
    if (lower == "r2" || lower == "rt") return 5;
    return -1;
}

inline bool parseUnsigned(std::string_view text, std::uint64_t& out) {
    if (text.empty()) return false;
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = value;
    return true;
}

}  // namespace detail

// Parses a script into events sorted by time (stable). Returns false and an explanation on a malformed script.
inline bool ParseVirtualPadScript(std::string_view script, std::vector<VirtualPadEvent>& events, std::string& error) {
    events.clear();
    std::size_t start = 0;
    while (start <= script.size()) {
        const auto end = script.find_first_of(";\n", start);
        const auto item = detail::trimView(script.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        start = end == std::string_view::npos ? script.size() + 1 : end + 1;
        if (item.empty() || item.front() == '#') continue;
        const auto colon = item.find(':');
        if (colon == std::string_view::npos) { error = "expected <when>:<op> in '" + std::string(item) + "'"; return false; }
        auto when = detail::trimView(item.substr(0, colon));
        const auto op = detail::trimView(item.substr(colon + 1));
        VirtualPadEvent base;
        if (!when.empty() && (when.front() == 'f' || when.front() == 'F')) { base.byFrame = true; when.remove_prefix(1); }
        if (!detail::parseUnsigned(when, base.when)) { error = "bad time in '" + std::string(item) + "'"; return false; }
        if (op.empty()) { error = "missing operation in '" + std::string(item) + "'"; return false; }
        const auto equals = op.find('=');
        if (op.front() == '+' || op.front() == '-') {
            const int button = detail::buttonIndex(detail::trimView(op.substr(1)));
            if (button < 0) { error = "unknown button in '" + std::string(item) + "'"; return false; }
            base.index = button;
            base.value = op.front() == '+' ? 1 : 0;
            events.push_back(base);
        } else if (equals != std::string_view::npos) {
            const auto name = detail::trimView(op.substr(0, equals));
            const std::string valueText(detail::trimView(op.substr(equals + 1)));
            if (detail::lowerCopy(name) == "shake") {
                char* shakeEnd = nullptr;
                const double seconds = std::strtod(valueText.c_str(), &shakeEnd);
                if (valueText.empty() || shakeEnd != valueText.c_str() + valueText.size() || seconds <= 0.0 || seconds > 30.0 ||
                    base.byFrame) {  // value holds ms in 16 bits
                    error = "bad shake duration '" + std::string(item) + "'";
                    return false;
                }
                base.isShake = true;
                base.value = static_cast<std::int16_t>(seconds * 1000.0);
                events.push_back(base);
            } else if (detail::lowerCopy(name) == "tap") {
                const int button = detail::buttonIndex(valueText);
                if (button < 0) { error = "unknown button in '" + std::string(item) + "'"; return false; }
                base.index = button;
                base.value = 1;
                events.push_back(base);
                base.when += base.byFrame ? 4 : 120;
                base.value = 0;
                base.tapRelease = true;
                events.push_back(base);
            } else {
                const int axis = detail::axisIndex(name);
                char* parsedEnd = nullptr;
                const double value = std::strtod(valueText.c_str(), &parsedEnd);
                if (axis < 0 || valueText.empty() || parsedEnd != valueText.c_str() + valueText.size() || value < -1.0 || value > 1.0) {
                    error = "bad axis assignment '" + std::string(item) + "'";
                    return false;
                }
                base.isAxis = true;
                base.index = axis;
                // A virtual trigger rests at the axis minimum (SDL maps raw -32768..32767 to 0..32767).
                const double scaled = axis >= 4 ? -32768.0 + std::max(value, 0.0) * 65535.0 : value * 32767.0;
                base.value = static_cast<std::int16_t>(std::clamp(scaled, -32768.0, 32767.0));
                events.push_back(base);
            }
        } else {
            error = "unknown operation in '" + std::string(item) + "'";
            return false;
        }
    }
    std::stable_sort(events.begin(), events.end(), [](const VirtualPadEvent& a, const VirtualPadEvent& b) {
        if (a.byFrame != b.byFrame) return !a.byFrame;
        return a.when < b.when;
    });
    return true;
}

}  // namespace Pad

#endif
