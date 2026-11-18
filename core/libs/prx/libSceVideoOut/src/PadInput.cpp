#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "SDL.h"
#include "prx/libSceVideoOut/include/PadInput.hpp"
#include "prx/libScePad/include/LatTrace.hpp"
#include "prx/libSceVideoOut/include/DualSenseStatus.hpp"
#include "prx/libScePad/include/PadTriggerEffect.hpp"
#include "prx/libSceVideoOut/include/PadStatusSdl.hpp"
#include "prx/libSceVideoOut/include/DualSenseHaptics.hpp"
#include "prx/libc/include/PreciseWait.hpp"
#include "prx/libSceMouse/include/MouseState.hpp"
#include "prx/libSceVideoOut/include/DisplayWindow.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/PadStatusOverlay.hpp"
#include "prx/libScePad/include/PadState.hpp"
#include "prx/libScePad/include/Pad.hpp"
#include "prx/libScePad/include/PadInputTypes.hpp"
#include "prx/libScePad/include/PadRumbleIntensity.hpp"
#include "prx/libSceUserService/UserService.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libSceIme/include/ImeInput.hpp"
#include "prx/libSceIme/include/ModalDialog.hpp"
#include "prx/libSceVideoOut/include/HostVolumeInput.hpp"
#include "prx/libScePad/include/HostVolumeSettings.hpp"
#include "prx/libScePad/include/HostVolumeControl.hpp"
#include "prx/libkernel/AppMetadata/include/AppMetadata.hpp"

namespace {

// SDL reports every virtual joystick as SDL_CONTROLLER_TYPE_VIRTUAL. A scripted or test pad that declares a Sony, Nintendo or
// Microsoft vendor/product id is treated as that pad, so the per-family handling can be exercised without hardware.
SDL_GameControllerType ControllerType(SDL_GameController* controller) {
    const auto type = SDL_GameControllerGetType(controller);
    if (type != SDL_CONTROLLER_TYPE_VIRTUAL) return type;
    const Uint16 vendor = SDL_GameControllerGetVendor(controller);
    const Uint16 product = SDL_GameControllerGetProduct(controller);
    if (vendor == 0x054c) return product == 0x0ce6 || product == 0x0df2 ? SDL_CONTROLLER_TYPE_PS5 : SDL_CONTROLLER_TYPE_PS4;
    if (vendor == 0x057e) return product == 0x2006 ? SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT : product == 0x2007 ? SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT : SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO;
    if (vendor == 0x045e) return SDL_CONTROLLER_TYPE_XBOXONE;
    return type;
}

void stopRumble(SDL_GameController* controller) {
    SDL_GameControllerRumble(controller, 0, 0, 0);
    if (SDL_GameControllerHasRumbleTriggers(controller) == SDL_TRUE) SDL_GameControllerRumbleTriggers(controller, 0, 0, 0);
}

bool publishInputMapping(Pad::InputMappingStore& mappings, const std::string& titleId) {
    Pad::InputConfiguration configuration;
    std::string error;
    if (!Pad::TryLoadInputMapping(titleId, configuration, error)) {
        APS5_LOG_ERR("Pad: input mapping reload rejected, the previous mapping stays active: %s", error.c_str());
        return false;
    }
    mappings.Publish(std::move(configuration));
    return true;
}

std::string appTitleId() {
    if (!std::filesystem::exists(ResolvePath_nid_no_patch("/app0/sce_sys/param.json")) && !std::filesystem::exists(ResolvePath_nid_no_patch("/app0/sce_sys/param.sfo"))) return {};
    const auto titleId = GetAppTitleId_nid_postfix();
    return std::string(titleId.value, std::find(titleId.value, titleId.value + sizeof(titleId.value), '\0'));
}

}

PadInput::PadInput() : PadInput(Pad::LoadInputMapping(appTitleId())) {
    HostVolumeConfigureFromEnvironment_nid_no_patch();
    startInputMappingWatch();
}

static_assert(PAD_MAX_CONTROLLERS == AgcDriver::PadStatusSlots, "the pad status panel lists one line per controller slot");

PadInput::PadInput(Pad::InputConfiguration configuration) : mappings(std::move(configuration)), appliedMapping(mappings.Snapshot()), keyboardMouse(*appliedMapping), statusProvider{[this] { return statusSnapshot(); }}, port0Mode(Port0Presence::ConfiguredMode()), port0(port0Mode, false) {
    {
        const char* log = std::getenv("NPS5_PAD_LOG");
        padLog = log != nullptr && *log != '\0' && std::string_view(log) != "0";
    }
    static_cast<void>(Pad::RumblePercent());
    static_cast<void>(Pad::RumblePausesOnFocusLoss());
    identifyMode = Pad::IdentifyModeFromEnvironment();
    openAvailableControllers();
    port0 = Port0Presence::Presence(port0Mode, slots[0].controller != nullptr);
    if (port0Mode == Port0Presence::Mode::Controller) applyPort0(port0.Connected());
}

PadInput::~PadInput() {
    scriptStop = true;
    if (scriptThread.joinable()) scriptThread.join();
    mappingWatchStop = true;
    if (mappingWatchThread.joinable()) mappingWatchThread.join();
    for (int i = 0; i < PAD_MAX_CONTROLLERS; ++i) {
        closeController(i);
    }
    if (scriptedJoystick != nullptr) SDL_JoystickClose(scriptedJoystick);
    if (scriptedDevice >= 0) SDL_JoystickDetachVirtual(scriptedDevice);
}

void PadInput::ConfigureSdl() {
    // Priority DEFAULT: an SDL_* variable in the environment still wins, so every driver can be switched off by hand.
    const auto hint = [](const char* name, const char* value) { SDL_SetHintWithPriority(name, value, SDL_HINT_DEFAULT); };
    hint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    hint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    hint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");           // DualSense: touchpad, gyro/accel, light bar, adaptive triggers
    hint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");    // DualSense rumble through the output report
    hint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "1");
    hint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    hint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");        // Switch Pro: HD rumble, gyro/accel, full button set
    hint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");
    // Positional face buttons: SDL_CONTROLLER_BUTTON_A is always the south button (Cross), also on Nintendo pads.
    hint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
}

void PadInput::openAvailableControllers() {
    ConfigureSdl();
    if ((SDL_WasInit(SDL_INIT_GAMECONTROLLER) & SDL_INIT_GAMECONTROLLER) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
            APS5_LOG_ERR("Pad: SDL game controller init failed: %s", SDL_GetError());
            return;
        }
    }
    attachScriptedPad();
    // The scripted pad takes the first free slot (slot 0 at start), ahead of any connected hardware, so an
    // unattended run drives the primary user whatever is plugged in.
    if (scriptedDevice >= 0) openController(scriptedDevice);
    for (int deviceIndex = 0; deviceIndex < SDL_NumJoysticks(); ++deviceIndex) {
        if (!SDL_IsGameController(deviceIndex)) continue;
        openController(deviceIndex);
    }
}

// NPS5_VPAD_SCRIPT: attach one SDL virtual game controller that follows the script (VirtualPadScript.hpp).
void PadInput::attachScriptedPad() {
    const char* text = std::getenv("NPS5_VPAD_SCRIPT");
    if (text == nullptr || *text == '\0' || scriptedDevice >= 0) return;
    std::string source = text;
    if (source.front() == '@') {
        std::ifstream file(source.substr(1));
        if (!file) {
            APS5_LOG_ERR("Pad: NPS5_VPAD_SCRIPT cannot read '%s'", source.c_str() + 1);
            return;
        }
        std::ostringstream content;
        content << file.rdbuf();
        source = content.str();
    }
    std::string error;
    if (!Pad::ParseVirtualPadScript(source, script, error)) {
        APS5_LOG_ERR("Pad: NPS5_VPAD_SCRIPT rejected: %s", error.c_str());
        script.clear();
        return;
    }
    SDL_VirtualJoystickDesc desc{};
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = 6;
    desc.nbuttons = 16;
    desc.name = "NPS5 Scripted Pad";
    if (const char* type = std::getenv("NPS5_VPAD_TYPE")) {
        const std::string kind = type;
        if (kind == "ps5") { desc.vendor_id = 0x054c; desc.product_id = 0x0ce6; desc.name = "NPS5 Scripted DualSense"; }
        else if (kind == "switch") { desc.vendor_id = 0x057e; desc.product_id = 0x2009; desc.name = "NPS5 Scripted Switch Pro"; }
        else if (kind == "xbox") { desc.vendor_id = 0x045e; desc.product_id = 0x0b12; desc.name = "NPS5 Scripted Xbox"; }
    }
    scriptedDevice = SDL_JoystickAttachVirtualEx(&desc);
    if (scriptedDevice < 0) {
        APS5_LOG_ERR("Pad: NPS5_VPAD_SCRIPT could not attach a virtual joystick: %s", SDL_GetError());
        return;
    }
    scriptedJoystick = SDL_JoystickOpen(scriptedDevice);
    if (scriptedJoystick != nullptr) {
        SDL_JoystickSetVirtualAxis(scriptedJoystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_JOYSTICK_AXIS_MIN);
        SDL_JoystickSetVirtualAxis(scriptedJoystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, SDL_JOYSTICK_AXIS_MIN);
    }
    APS5_LOG_OUT("Pad: scripted virtual pad attached (%zu events, NPS5_VPAD_SCRIPT)", script.size());
}

void PadInput::notePress(int index, std::uint64_t elapsedMs) {
    if (index < 0 || static_cast<std::size_t>(index) >= tapStamps.size()) return;
    tapStamps[index] = {elapsedMs, LatTrace::gGuestReads.load()};
}

// nps5: a tap is held until the title has had time to sample it: 100 ms and 2 guest pad reads after ITS press, but never longer than 2 s
// (a guest that does not poll, e.g. a blocked loading screen, must not freeze the rest of the script).
bool PadInput::tapHeldLongEnough(int index, std::uint64_t elapsedMs) {
    if (index < 0 || static_cast<std::size_t>(index) >= tapStamps.size()) return true;
    const auto& stamp = tapStamps[index];
    if (elapsedMs < stamp.ms + 100) return false;
    if (LatTrace::gGuestReads.load() >= stamp.reads + 2) return true;
    if (elapsedMs < stamp.ms + 2000) return false;
    if (!tapCapLogged) {
        tapCapLogged = true;
        std::fprintf(stderr, "[pad] script: tap on button %d released after the 2 s cap (the guest did not read the pad twice)\n", index);
    }
    return true;
}

void PadInput::advanceScriptedPad(const DisplayWindow& window) {
    if (scriptedJoystick == nullptr || scriptNext >= script.size()) return;
    if (!scriptStarted) {
        scriptStarted = true;
        scriptStart = std::chrono::steady_clock::now();
        scriptStartFrame = window.FrameNumber();
    }
    // nps5: NPS5_VPAD_ASYNC=1 applies the millisecond events from their own thread at their own time, like a real
    // controller's report arriving between two samples (otherwise the very Update that applies an event also
    // samples it, which hides the sampling delay from APS5_TRACE_LAT).
    if (!scriptThread.joinable() && std::getenv("NPS5_VPAD_ASYNC") != nullptr &&
        std::none_of(script.begin(), script.end(), [](const auto& e) { return e.byFrame; })) {
        scriptThread = std::thread([this] {
            for (const auto& event : script) {
                const auto at = scriptStart + std::chrono::milliseconds(event.when);
                while (!scriptStop && std::chrono::steady_clock::now() < at) std::this_thread::sleep_for(std::chrono::microseconds(200));
                if (scriptStop) return;
                if (event.isShake) continue;
                const auto asyncMs = [&] { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - scriptStart).count()); };
                if (event.tapRelease && !event.isAxis) {
                    while (!scriptStop && !tapHeldLongEnough(event.index, asyncMs())) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    if (scriptStop) return;
                } else if (!event.isAxis && event.value != 0) {
                    notePress(event.index, asyncMs());
                }
                if (event.value != 0 && (!event.isAxis || LatTrace::AxisInject())) LatTrace::Inject();
                if (event.isAxis) SDL_JoystickSetVirtualAxis(scriptedJoystick, event.index, event.value);
                else SDL_JoystickSetVirtualButton(scriptedJoystick, event.index, static_cast<Uint8>(event.value));
            }
        });
        scriptNext = script.size();
        return;
    }
    const auto elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - scriptStart).count());
    const std::uint64_t elapsedFrames = window.FrameNumber() - scriptStartFrame;
    while (scriptNext < script.size()) {
        const auto& event = script[scriptNext];
        if ((event.byFrame ? elapsedFrames : elapsedMs) < event.when) break;
        if (event.isShake) {
            ++scriptNext;
            continue;
        }  // shake=<s> plays in the Kyty host only
        // nps5: a tap is held until the title has had time to sample it: 100 ms and 2 guest pad reads after the press
        // (a frame-counted tap is 4 presented flips, which at a fast loading screen is a few ms and can be read
        // by nobody).
        if (event.tapRelease && !event.isAxis) {
            if (!tapHeldLongEnough(event.index, elapsedMs)) break;
        } else if (!event.isAxis && event.value != 0) {
            notePress(event.index, elapsedMs);
        }
        if (event.value != 0 && (!event.isAxis || LatTrace::AxisInject())) LatTrace::Inject();
        if (event.isAxis) SDL_JoystickSetVirtualAxis(scriptedJoystick, event.index, event.value);
        else SDL_JoystickSetVirtualButton(scriptedJoystick, event.index, static_cast<Uint8>(event.value));
        if (padLog) std::fprintf(stderr, "[pad] script: %s %d = %d (t=%llums frame+%llu)\n", event.isAxis ? "axis" : "button", event.index, event.value,
            static_cast<unsigned long long>(elapsedMs), static_cast<unsigned long long>(elapsedFrames));
        ++scriptNext;
    }
}

void PadInput::logPadState(int slot) {
    const auto& now = slots[slot].state;
    auto& before = slots[slot].logged;
    const auto near = [](std::uint8_t a, std::uint8_t b) { return a / 8 == b / 8; };
    const bool same = slots[slot].loggedValid && now.buttons == before.buttons && near(now.sticks[0], before.sticks[0]) && near(now.sticks[1], before.sticks[1]) &&
        near(now.sticks[2], before.sticks[2]) && near(now.sticks[3], before.sticks[3]) && near(now.analogButtonsL2, before.analogButtonsL2) &&
        near(now.analogButtonsR2, before.analogButtonsR2) && now.touch[0].active == before.touch[0].active && now.touch[1].active == before.touch[1].active;
    if (same) return;
    before = now;
    slots[slot].loggedValid = true;
    std::fprintf(stderr, "[pad] slot %d (%s): buttons=0x%06x L=(%u,%u) R=(%u,%u) L2=%u R2=%u touch=%d%d%s gyro=(%.2f,%.2f,%.2f)\n", slot, slots[slot].name.c_str(), now.buttons,
        now.sticks[0], now.sticks[1], now.sticks[2], now.sticks[3], now.analogButtonsL2, now.analogButtonsR2, now.touch[0].active, now.touch[1].active,
        now.hasMotion ? " motion" : "", now.gyro[0], now.gyro[1], now.gyro[2]);
}

void PadInput::openController(int deviceIndex) {
    if (!SDL_IsGameController(deviceIndex)) return;
    const auto instanceId = SDL_JoystickGetDeviceInstanceID(deviceIndex);
    for (int i = 0; i < PAD_MAX_CONTROLLERS; ++i) {
        if (slots[i].controller != nullptr && slots[i].instanceId == instanceId) return;
    }
    int slot = -1;
    for (int i = 0; i < PAD_MAX_CONTROLLERS; ++i) {
        if (slots[i].controller == nullptr) {
            slot = i;
            break;
        }
    }
    if (slot == -1) return;
    SDL_GameController* gc = SDL_GameControllerOpen(deviceIndex);
    if (gc == nullptr) {
        APS5_LOG_ERR("Pad: could not open game controller %d: %s", deviceIndex, SDL_GetError());
        return;
    }
    slots[slot].controller = gc;
    slots[slot].instanceId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
    slots[slot].state = {};
    slots[slot].outputSequence = 0;
    slots[slot].outputState = {};
    slots[slot].outputPending = true;
    slots[slot].rumbleLive = false;
    slots[slot].nextRumbleRefresh = {};
    slots[slot].lightBar.Reset();
    slots[slot].triggerEffect.Reset();
    openStatus(slot);
    slots[slot].identify = Pad::IdentifyScheduler{};
    const char* name = SDL_GameControllerName(gc);
    slots[slot].kind = PadKindFromSdl(ControllerType(gc));
    slots[slot].name = name != nullptr ? name : "";
    APS5_LOG_OUT("Pad: connected game controller (slot %d): %s (type %d, sensors accel=%d gyro=%d, touchpads=%d, led=%d, trigger rumble=%d)",
        slot, name != nullptr ? name : "unknown", static_cast<int>(ControllerType(gc)),
        SDL_GameControllerHasSensor(gc, SDL_SENSOR_ACCEL) == SDL_TRUE, SDL_GameControllerHasSensor(gc, SDL_SENSOR_GYRO) == SDL_TRUE,
        SDL_GameControllerGetNumTouchpads(gc), SDL_GameControllerHasLED(gc) == SDL_TRUE, SDL_GameControllerHasRumbleTriggers(gc) == SDL_TRUE);
    if (identifyMode != Pad::IdentifyMode::Off) {
        if (SDL_GameControllerHasRumble(gc) == SDL_TRUE) {
            slots[slot].identify.Bind(std::chrono::steady_clock::now(), static_cast<unsigned>(slot) + 1, identifyMode);
        } else {
            APS5_LOG_OUT("Pad: identify pulse skipped for slot %d: the controller has no rumble support", slot);
        }
    }
    enableSensors(slot);
    if (slot > 0) {
        PadSetPortConnected_nid_postfix(slot, true);
        UserServiceSetUserConnected_nid_postfix(slot, true);
    }
    if (slot == 0) openHaptics(gc);
}

void PadInput::enableSensors(int slot) {
    if (slot < 0 || slot >= PAD_MAX_CONTROLLERS) return;
    SDL_GameController* controller = slots[slot].controller;
    if (controller == nullptr) return;
    const SDL_bool wanted = slots[slot].outputState.motionEnabled ? SDL_TRUE : SDL_FALSE;
    if (SDL_GameControllerHasSensor(controller, SDL_SENSOR_ACCEL) == SDL_TRUE) SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_ACCEL, wanted);
    if (SDL_GameControllerHasSensor(controller, SDL_SENSOR_GYRO) == SDL_TRUE) SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_GYRO, wanted);
}

void PadInput::openStatus(int slot) {
    if (slot < 0 || slot >= PAD_MAX_CONTROLLERS) return;
    PadClearTriggerStatus_nid_postfix(slot);
    SDL_GameController* controller = slots[slot].controller;
    if (controller == nullptr || ControllerType(controller) != SDL_CONTROLLER_TYPE_PS5) return;
    const char* path = SDL_GameControllerPath(controller);
    if (path == nullptr) return;
    slots[slot].statusHid = SDL_hid_open_path(path, 0);
    if (slots[slot].statusHid == nullptr) {
        APS5_LOG_ERR("Pad: could not open the DualSense trigger state report (slot %d): %s", slot, SDL_GetError());
    }
}

void PadInput::pollStatus(int slot) {
    if (slot < 0 || slot >= PAD_MAX_CONTROLLERS || slots[slot].statusHid == nullptr) return;
    std::array<std::uint8_t, 78> report{};
    while (true) {
        const int read = SDL_hid_read_timeout(slots[slot].statusHid, report.data(), report.size(), 0);
        if (read == 0) return;
        if (read < 0) {
            APS5_LOG_ERR("Pad: could not read the DualSense trigger state report (slot %d): %s", slot, SDL_GetError());
            closeStatus(slot);
            return;
        }
        DualSenseStatus::TriggerStatus status;
        if (!DualSenseStatus::Parse(report.data(), static_cast<std::size_t>(read), status)) continue;
        PadPublishTriggerStatus_nid_postfix(slot, status.leftEffect, status.leftStatus, status.rightEffect, status.rightStatus);
    }
}

void PadInput::closeStatus(int slot) {
    if (slot < 0 || slot >= PAD_MAX_CONTROLLERS) return;
    PadClearTriggerStatus_nid_postfix(slot);
    if (slots[slot].statusHid == nullptr) return;
    SDL_hid_close(slots[slot].statusHid);
    slots[slot].statusHid = nullptr;
}

void PadInput::openHaptics(SDL_GameController* controller) {
    if (controller == nullptr || ControllerType(controller) != SDL_CONTROLLER_TYPE_PS5) return;
    const char* path = SDL_GameControllerPath(controller);
    if (path == nullptr) return;
    bool bluetooth = false;
    SDL_hid_device_info* devices = SDL_hid_enumerate(0x054c, 0);
    for (auto* device = devices; device != nullptr; device = device->next) {
        if (device->path != nullptr && std::strcmp(device->path, path) == 0) bluetooth = device->interface_number == -1;
    }
    SDL_hid_free_enumeration(devices);
    if (!bluetooth) return;
    haptics = SDL_hid_open_path(path, 0);
    if (haptics == nullptr) {
        APS5_LOG_ERR("Pad: could not open the Bluetooth DualSense for haptics: %s", SDL_GetError());
        return;
    }
    hapticsRunning = true;
    PadSetHapticStreamOpen_nid_postfix(true);
    hapticsThread = std::thread([this] { streamHaptics(); });
    APS5_LOG_CHARS_OUT("Pad: vibration ports play on the Bluetooth DualSense as haptics reports");
}

void PadInput::streamHaptics() {
    std::uint8_t sequence = 0;
    std::uint8_t counter = 0;
    const auto write = [this](std::span<const std::uint8_t> report) {
        if (SDL_hid_write(haptics, report.data(), report.size()) >= 0) return true;
        APS5_LOG_ERR("Pad: a Bluetooth DualSense haptics report failed (%s); haptics stop until the controller reconnects", SDL_GetError());
        hapticsRunning = false;
        return false;
    };
    if (!write(DualSenseHaptics::PowerReport(sequence++))) return;
    auto next = std::chrono::steady_clock::now() - DualSenseHaptics::ReportInterval;
    while (hapticsRunning) {
        std::array<std::int8_t, DualSenseHaptics::SamplesBytes> samples{};
        if (!PadWaitTakeHapticBlock_nid_postfix(samples.data(), 50)) continue;
        if (std::all_of(samples.begin(), samples.end(), [](std::int8_t sample) { return sample == 0; })) continue;
        next = DualSenseHaptics::NextReport(next, std::chrono::steady_clock::now());
        PreciseSleepUntil(next);
        if (const auto due = hapticsReconfigureAt.load(); due != 0 && std::chrono::steady_clock::now().time_since_epoch().count() >= due) {
            hapticsReconfigureAt = 0;
            if (!write(DualSenseHaptics::PowerReport(sequence++))) return;
        }
        if (!write(DualSenseHaptics::Report(sequence++, counter++, samples))) return;
    }
}

void PadInput::closeHaptics() {
    if (haptics == nullptr) return;
    PadSetHapticStreamOpen_nid_postfix(false);
    hapticsRunning = false;
    if (hapticsThread.joinable()) hapticsThread.join();
    SDL_hid_close(haptics);
    haptics = nullptr;
    hapticsRumbled = false;
    hapticsReconfigureAt = 0;
}

void PadInput::closeController(int slot) {
    if (slot < 0 || slot >= PAD_MAX_CONTROLLERS) return;
    closeStatus(slot);
    if (slot == 0) closeHaptics();
    if (slots[slot].controller == nullptr) return;
    SDL_GameControllerClose(slots[slot].controller);
    slots[slot].controller = nullptr;
    slots[slot].instanceId = -1;
    slots[slot].kind = AgcDriver::PadKind::Unknown;
    slots[slot].name.clear();
    slots[slot].state = {};
    slots[slot].outputState = {};
    slots[slot].outputPending = false;
    if (slot > 0) {
        PadSetPortConnected_nid_postfix(slot, false);
        UserServiceSetUserConnected_nid_postfix(slot, false);
    }
}

AgcDriver::PadStatusSnapshot PadInput::statusSnapshot() const {
    AgcDriver::PadStatusSnapshot snapshot;
    for (int slot = 0; slot < PAD_MAX_CONTROLLERS; ++slot) {
        SDL_GameController* controller = slots[slot].controller;
        if (controller == nullptr) continue;
        auto& status = snapshot.slots[static_cast<std::size_t>(slot)];
        status.connected = true;
        status.kind = slots[slot].kind;
        status.name = slots[slot].name;
        status.power = PadPowerFromSdl(SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(controller)));
    }
    return snapshot;
}

void PadInput::reportStatus() {
    AgcDriverPadStatusPoll_nid_postfix(&statusProvider);
}

void PadInput::applyPort0(bool connected) {
    PadSetPortConnected_nid_postfix(0, connected);
    UserServiceSetUserConnected_nid_postfix(0, connected);
}

void PadInput::updatePort0() {
    if (const auto change = port0.Observe(slots[0].controller != nullptr, std::chrono::steady_clock::now())) applyPort0(*change);
}

void PadInput::applyOutput() {
    for (int slot = 0; slot < PAD_MAX_CONTROLLERS; ++slot) {
        PadOutputState fetched;
        if (PadFetchOutputForPort_nid_postfix(slot, &slots[slot].outputSequence, &fetched)) {
            const bool motionChanged = fetched.motionEnabled != slots[slot].outputState.motionEnabled;
            slots[slot].outputState = fetched;
            slots[slot].outputPending = true;
            if (motionChanged) enableSensors(slot);
        }
        SDL_GameController* controller = slots[slot].controller;
        if (controller == nullptr) continue;
        const auto now = std::chrono::steady_clock::now();
        const bool isPs5 = ControllerType(controller) == SDL_CONTROLLER_TYPE_PS5;
        const auto large = std::max(slots[slot].outputState.vibrationLarge, slots[slot].outputState.audioVibrationLarge);
        const auto small = std::max(slots[slot].outputState.vibrationSmall, slots[slot].outputState.audioVibrationSmall);
        const unsigned rumblePercent = Pad::RumblePercent();
        const auto motors = Pad::MakeRumbleCommand(large, small, rumblePercent, 2000);
        const auto triggers = Pad::MakeRumbleCommand(slots[slot].outputState.trigger[0].fallback, slots[slot].outputState.trigger[1].fallback, rumblePercent, 2000);
        const bool rumbling = motors.Active();
        const bool triggerRumble = triggers.Active();
        const bool triggerTouched = slots[slot].outputState.triggerTouched[0] || slots[slot].outputState.triggerTouched[1];
        TriggerEffectCache::Effect effect{};
        if (isPs5 && triggerTouched) effect = PadTriggerEffect::Build(slots[slot].outputState);
        const bool effectPending = isPs5 && triggerTouched && slots[slot].triggerEffect.NeedsSend(effect);
        if (SDL_GameControllerHasLED(controller) == SDL_TRUE) {
            struct PlayerLedColor { Uint8 r, g, b; };
            static constexpr PlayerLedColor defaultColors[4] = {
                {0, 64, 255},
                {255, 0, 0},
                {0, 255, 0},
                {255, 0, 128}
            };
            const auto& def = defaultColors[slot % 4];
            const LightBarPacer::Color color = slots[slot].outputState.lightBarValid
                ? LightBarPacer::Color{slots[slot].outputState.lightBar[0], slots[slot].outputState.lightBar[1], slots[slot].outputState.lightBar[2]}
                : LightBarPacer::Color{def.r, def.g, def.b};
            if (slots[slot].lightBar.Due(color, now)) slots[slot].lightBar.Result(color, now, SDL_GameControllerSetLED(controller, color[0], color[1], color[2]) == 0);
        }
        const bool suppressed = Pad::RumbleSuppressed(Pad::RumblePausesOnFocusLoss(), HostVolumeFocused_nid_no_patch());
        if (suppressed && slots[slot].rumbleLive) {
            stopRumble(controller);
            slots[slot].rumbleLive = false;
        }
        const bool titleActive = suppressed || slots[slot].outputPending || rumbling || triggerRumble;
        if (slots[slot].identify.Tick(now, titleActive, rumblePercent, [controller](std::uint16_t low, std::uint16_t high, std::uint32_t durationMs) {
                SDL_GameControllerRumble(controller, low, high, durationMs);
            })) continue;
        if (!slots[slot].outputPending) {
            if (effectPending || ((rumbling || (triggerRumble && !isPs5)) && now >= slots[slot].nextRumbleRefresh)) slots[slot].outputPending = true;
            else continue;
        }
        if (!suppressed) {
            slots[slot].outputPending = false;
            slots[slot].nextRumbleRefresh = now + std::chrono::milliseconds(700);
            SDL_GameControllerRumble(controller, motors.low, motors.high, motors.durationMs);
            slots[slot].rumbleLive = rumbling || triggerRumble;
        }
        if (slot == 0 && haptics != nullptr) {
            if (hapticsRumbled && !rumbling) hapticsReconfigureAt = (now + DualSenseHaptics::RumbleSettle).time_since_epoch().count();
            hapticsRumbled = rumbling;
        }
        if (triggerTouched) {
            if (isPs5) {
                if (slots[slot].triggerEffect.NeedsSend(effect)) {
                    slots[slot].triggerEffect.Result(effect, SDL_GameControllerSendEffect(controller, effect.data(), static_cast<int>(effect.size())) == 0);
                }
            } else if (!suppressed && SDL_GameControllerHasRumbleTriggers(controller) == SDL_TRUE) {
                SDL_GameControllerRumbleTriggers(controller, triggers.low, triggers.high, triggers.durationMs);
            }
        }
    }
}

void PadInput::applyMappingSnapshot() {
    auto snapshot = mappings.Snapshot();
    if (snapshot == appliedMapping) return;
    appliedMapping = snapshot;
    keyboardMouse.Replace(*snapshot);
}

void PadInput::reloadInputMapping() {
    if (!publishInputMapping(mappings, appTitleId())) return;
    applyMappingSnapshot();
    APS5_LOG_CHARS_OUT("Pad: input mapping reloaded");
}

void PadInput::startInputMappingWatch() {
    const char* enabled = std::getenv("ANYPS5_INPUT_WATCH");
    if (enabled == nullptr || std::string_view(enabled) != "1") return;
    auto titleId = appTitleId();
    auto watch = Pad::InputMappingWatch(Pad::InputMappingFileStamp(titleId));
    mappingWatchThread = std::thread([this, titleId = std::move(titleId), watch = std::move(watch)]() mutable {
        watchInputMapping(titleId, std::move(watch));
    });
    APS5_LOG_CHARS_OUT("Pad: watching the input mapping for changes (ANYPS5_INPUT_WATCH=1)");
}

void PadInput::watchInputMapping(std::string titleId, Pad::InputMappingWatch watch) {
    while (!mappingWatchStop) {
        for (int slice = 0; slice < 10 && !mappingWatchStop; ++slice) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (mappingWatchStop) return;
        if (!watch.Poll(Pad::InputMappingFileStamp(titleId))) continue;
        if (publishInputMapping(mappings, titleId)) APS5_LOG_CHARS_OUT("Pad: input mapping reloaded from a file change");
    }
}

void PadInput::setMouseMode(bool enabled) {
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "0");
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0");
    if (SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE) != 0) throw std::runtime_error(std::string("Pad: relative mouse mode failed: ") + SDL_GetError());
    keyboardMouse.SetMouseMode(enabled);
}

PadInputState PadInput::sampleController(int slot) const {
    PadInputState result;
    if (slot < 0 || slot >= PAD_MAX_CONTROLLERS) return result;
    SDL_GameController* controller = slots[slot].controller;
    if (controller == nullptr) return result;
    const auto readButton = [controller](SDL_GameControllerButton button) {
        return SDL_GameControllerGetButton(controller, button) != 0;
    };
    const auto addButton = [&result, &readButton](SDL_GameControllerButton source, Pad::PadButton button) {
        if (readButton(source)) result.buttons |= static_cast<std::uint32_t>(button);
    };
    addButton(SDL_CONTROLLER_BUTTON_A, Pad::PadButton::Cross);
    addButton(SDL_CONTROLLER_BUTTON_B, Pad::PadButton::Circle);
    addButton(SDL_CONTROLLER_BUTTON_X, Pad::PadButton::Square);
    addButton(SDL_CONTROLLER_BUTTON_Y, Pad::PadButton::Triangle);
    addButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, Pad::PadButton::L1);
    addButton(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, Pad::PadButton::R1);
    const auto type = ControllerType(controller);
    // Non-Sony pads have no touchpad: View/Back (Xbox), Minus and Capture (Switch) click it at its centre.
    const bool nintendo = type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO || type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
        type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT || type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
    const bool viewPressed = type != SDL_CONTROLLER_TYPE_PS4 && type != SDL_CONTROLLER_TYPE_PS5 &&
        (readButton(SDL_CONTROLLER_BUTTON_BACK) || (nintendo && readButton(SDL_CONTROLLER_BUTTON_MISC1)));
    if (viewPressed) result.buttons |= static_cast<std::uint32_t>(Pad::PadButton::TouchPad);
    addButton(SDL_CONTROLLER_BUTTON_START, Pad::PadButton::Options);
    addButton(SDL_CONTROLLER_BUTTON_LEFTSTICK, Pad::PadButton::L3);
    addButton(SDL_CONTROLLER_BUTTON_RIGHTSTICK, Pad::PadButton::R3);
    addButton(SDL_CONTROLLER_BUTTON_DPAD_UP, Pad::PadButton::Up);
    addButton(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, Pad::PadButton::Right);
    addButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, Pad::PadButton::Down);
    addButton(SDL_CONTROLLER_BUTTON_DPAD_LEFT, Pad::PadButton::Left);
    addButton(SDL_CONTROLLER_BUTTON_TOUCHPAD, Pad::PadButton::TouchPad);

    const auto triggerValue = [controller](SDL_GameControllerAxis axis) {
        const auto value = std::clamp<int>(SDL_GameControllerGetAxis(controller, axis), 0, 32767);
        return static_cast<std::uint8_t>((value * 255 + 16383) / 32767);
    };
    result.analogButtonsL2 = triggerValue(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    result.analogButtonsR2 = triggerValue(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    if (result.analogButtonsL2 != 0) result.buttons |= static_cast<std::uint32_t>(Pad::PadButton::L2);
    if (result.analogButtonsR2 != 0) result.buttons |= static_cast<std::uint32_t>(Pad::PadButton::R2);

    const auto stick = [this, controller](SDL_GameControllerAxis xAxis, SDL_GameControllerAxis yAxis, int deadzonePercent) {
        return Pad::StickWithDeadzone(SDL_GameControllerGetAxis(controller, xAxis), SDL_GameControllerGetAxis(controller, yAxis), deadzonePercent);
    };
    const auto left = stick(SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, keyboardMouse.Configuration().settings.leftStickDeadzonePercent);
    const auto right = stick(SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, keyboardMouse.Configuration().settings.rightStickDeadzonePercent);
    result.sticks = {left[0], left[1], right[0], right[1]};
    switch (ControllerType(controller)) {
        case SDL_CONTROLLER_TYPE_PS5: result.deviceKind = 1; break;
        case SDL_CONTROLLER_TYPE_PS4: result.deviceKind = 2; break;
        default: result.deviceKind = 3; break;
    }
    if (SDL_GameControllerIsSensorEnabled(controller, SDL_SENSOR_ACCEL) == SDL_TRUE && SDL_GameControllerIsSensorEnabled(controller, SDL_SENSOR_GYRO) == SDL_TRUE) {
        float accel[3];
        float gyro[3];
        if (SDL_GameControllerGetSensorData(controller, SDL_SENSOR_ACCEL, accel, 3) == 0 && SDL_GameControllerGetSensorData(controller, SDL_SENSOR_GYRO, gyro, 3) == 0) {
            result.hasMotion = true;
            for (int i = 0; i < 3; ++i) { result.accel[i] = accel[i]; result.gyro[i] = gyro[i]; }
        }
    }
    if (SDL_GameControllerGetNumTouchpads(controller) > 0) {
        for (int finger = 0; finger < 2; ++finger) {
            Uint8 down = 0;
            float x = 0.0f;
            float y = 0.0f;
            float pressure = 0.0f;
            if (SDL_GameControllerGetTouchpadFinger(controller, 0, finger, &down, &x, &y, &pressure) != 0 || down == 0) continue;
            result.touch[finger].active = true;
            result.touch[finger].x = static_cast<std::uint16_t>(std::clamp(x, 0.0f, 1.0f) * (kTouchPadWidth - 1));
            result.touch[finger].y = static_cast<std::uint16_t>(std::clamp(y, 0.0f, 1.0f) * (kTouchPadHeight - 1));
        }
    }
    if (viewPressed && !result.touch[0].active && !result.touch[1].active) {
        result.touch[0] = {true, kTouchPadWidth / 2, kTouchPadHeight / 2, 0};
    }
    return result;
}

void PadInput::HandleEvent(const SDL_Event& event, DisplayWindow& window) {
    if (event.type == SDL_CONTROLLERBUTTONDOWN) LatTrace::Event();
    applyMappingSnapshot();
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
        openController(event.cdevice.which);
        return;
    }
    if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
        for (int slot = 0; slot < PAD_MAX_CONTROLLERS; ++slot) {
            if (slots[slot].controller != nullptr && slots[slot].instanceId == event.cdevice.which) {
                closeController(slot);
                publish(window.FrameNumber(), slot);
                break;
            }
        }
        return;
    }
    if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP ||
        event.type == SDL_CONTROLLERAXISMOTION || event.type == SDL_CONTROLLERDEVICEREMAPPED) {
        SDL_JoystickID which = -1;
        if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP) which = event.cbutton.which;
        else if (event.type == SDL_CONTROLLERAXISMOTION) which = event.caxis.which;
        else if (event.type == SDL_CONTROLLERDEVICEREMAPPED) which = event.cdevice.which;
        for (int slot = 0; slot < PAD_MAX_CONTROLLERS; ++slot) {
            if (slots[slot].controller != nullptr && slots[slot].instanceId == which) {
                slots[slot].state = sampleController(slot);
                if (slot == 0 && (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP)) dispatchPadChords(window);
                publish(window.FrameNumber(), slot);
                return;
            }
        }
        return;
    }
    if (event.type == SDL_WINDOWEVENT) {
        if (keyboardMouse.HandleEvent(event, std::chrono::steady_clock::now()).releaseMouse) setMouseMode(false);
        HostVolume volume;
        if (window.Handle() != nullptr && ApplyHostFocusEvent(event, SDL_GetWindowID(window.Handle()), &volume)) APS5_LOG_ERR("%s", HostVolumeMessage(volume).c_str());
        publish(window.FrameNumber(), 0);
        return;
    }
    // APS5_NO_PAD_INPUT=1 keeps a measurement run from reacting to keys or mouse buttons that reach
    // its window (a stray press advances the title into another stage).
    static const bool ignoreInput = std::getenv("APS5_NO_PAD_INPUT") != nullptr;
    const bool input = event.type == SDL_MOUSEWHEEL || event.type == SDL_MOUSEMOTION || event.type == SDL_KEYDOWN || event.type == SDL_KEYUP ||
        event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP;
    if (!input || ignoreInput) return;
    if (event.type == SDL_KEYDOWN && ImeCapturesKeyboard_nid_postfix()) return;
    if (event.type != SDL_KEYUP && event.type != SDL_MOUSEBUTTONUP && ModalDialogCapturesInput_nid_postfix()) return;
    keyboardMouse.SetTitleReadsMouse(MouseIsOpen_nid_no_patch());
    static const bool mouseTouchpad = std::getenv("ANYPS5_MOUSE_TOUCHPAD") != nullptr && std::string_view(std::getenv("ANYPS5_MOUSE_TOUCHPAD")) == "1";
    keyboardMouse.SetMouseTouchpad(mouseTouchpad);
    static const bool mouseGyro = std::getenv("ANYPS5_MOUSE_GYRO") != nullptr && std::string_view(std::getenv("ANYPS5_MOUSE_GYRO")) == "1";
    keyboardMouse.SetMouseGyro(mouseGyro);
    int viewportWidth = 0;
    int viewportHeight = 0;
    if (window.Handle() != nullptr) SDL_GetWindowSize(window.Handle(), &viewportWidth, &viewportHeight);
    keyboardMouse.SetViewport(viewportWidth, viewportHeight);
    const auto requests = keyboardMouse.HandleEvent(event, std::chrono::steady_clock::now());
    const bool targetsWindow = window.Handle() != nullptr && event.key.windowID == SDL_GetWindowID(window.Handle());
    applyRequests(requests, targetsWindow, window);
    if (requests.toggleMouse) setMouseMode(!keyboardMouse.MouseMode());
    if (event.type != SDL_MOUSEMOTION) publish(window.FrameNumber(), 0);
}

void PadInput::dispatchPadChords(DisplayWindow& window) {
    if (ImeCapturesPad_nid_postfix() || ModalDialogCapturesInput_nid_postfix()) return;
    const auto requests = keyboardMouse.PadChordRequests(slots[0].state.buttons);
    const bool focused = window.Handle() != nullptr && SDL_GetKeyboardFocus() == window.Handle();
    applyRequests(requests, focused, window);
}

void PadInput::applyRequests(const Pad::InputRequests& requests, bool windowFocused, DisplayWindow& window) {
    if (requests.reloadInputMapping) reloadInputMapping();
    if (requests.toggleFullscreen && windowFocused) window.ToggleFullscreen();
    if (requests.togglePerformanceOverlay && windowFocused) AgcDriverTogglePerformanceOverlay_nid_postfix();
    if (requests.screenshot && windowFocused) AgcDriverRequestScreenshot_nid_postfix();
    if (requests.showPadStatus && windowFocused) {
        AgcDriverPadStatusShow_nid_postfix();
        reportStatus();
    }
    if (windowFocused) {
        HostVolume volume;
        if (ApplyHostVolumeRequests(requests, &volume)) APS5_LOG_ERR("%s", HostVolumeMessage(volume).c_str());
    }
}

void PadInput::Update(const DisplayWindow& window) {
    applyMappingSnapshot();
    if (!hostFocusSynced && window.Handle() != nullptr) {
        hostFocusSynced = true;
        HostVolume volume;
        if (HostVolumeFocus_nid_no_patch(SDL_GetKeyboardFocus() == window.Handle(), &volume)) APS5_LOG_ERR("%s", HostVolumeMessage(volume).c_str());
    }
    bool anyController = false;
    for (int slot = 0; slot < PAD_MAX_CONTROLLERS; ++slot) {
        if (slots[slot].controller != nullptr) {
            anyController = true;
            break;
        }
    }
    advanceScriptedPad(window);
    if (anyController) SDL_GameControllerUpdate();
    for (int slot = 0; slot < PAD_MAX_CONTROLLERS; ++slot) {
        if (slots[slot].controller != nullptr) pollStatus(slot);
    }
    updatePort0();
    reportStatus();
    applyOutput();
    for (int slot = 0; slot < PAD_MAX_CONTROLLERS; ++slot) {
        if (slots[slot].controller != nullptr) {
            slots[slot].state = sampleController(slot);
            if (padLog) logPadState(slot);
        }
    }
    if (keyboardMouse.MouseMode() && SDL_GetKeyboardFocus() == nullptr) {
        keyboardMouse.Release();
        setMouseMode(false);
        publish(window.FrameNumber(), 0);
        return;
    }
    const bool changed = keyboardMouse.Advance(std::chrono::steady_clock::now());
    const bool captured = ImeCapturesPad_nid_postfix() || ModalDialogCapturesInput_nid_postfix();
    const bool captureChanged = std::exchange(padCaptured, captured) != captured;
    if (slots[0].controller != nullptr || keyboardMouse.MouseMode() || changed || captured || captureChanged) {
        publish(window.FrameNumber(), 0);
    }
    for (int slot = 1; slot < PAD_MAX_CONTROLLERS; ++slot) {
        if (slots[slot].controller != nullptr) {
            publish(window.FrameNumber(), slot);
        }
    }
}

void PadInput::publish(std::uint64_t frame) {
    for (int slot = 0; slot < PAD_MAX_CONTROLLERS; ++slot) {
        publish(frame, slot);
    }
}

PadInputState PadInput::handOff(int slot, PadInputState state) {
    auto& held = slots[slot].heldThroughCapture;
    const bool imeCaptures = ImeCapturesPad_nid_postfix();
    if (imeCaptures || ModalDialogCapturesInput_nid_postfix()) {
        if (slot == 0) PadApplyInjectedInput_nid_postfix(&state);
        if (slot == 0 && imeCaptures) ImePublishPad_nid_postfix({state.buttons, state.sticks[0], state.sticks[1], state.analogButtonsL2});
        else if (slot == 0) ModalDialogPublishPad_nid_postfix({state.buttons, state.sticks[0], state.sticks[1], state.analogButtonsL2});
        held = state.buttons;
        PadInputState neutral;
        neutral.hasMotion = false;
        neutral.deviceKind = state.deviceKind;
        neutral.intercepted = true;
        return neutral;
    }
    held &= state.buttons;
    state.buttons &= ~held;
    if ((held & static_cast<std::uint32_t>(Pad::PadButton::L2)) != 0) state.analogButtonsL2 = 0;
    if ((held & static_cast<std::uint32_t>(Pad::PadButton::R2)) != 0) state.analogButtonsR2 = 0;
    return state;
}

void PadInput::publish(std::uint64_t frame, int slot) {
    if (slot < 0 || slot >= PAD_MAX_CONTROLLERS) return;
    if (slot == 0) {
        const auto composed = port0.Connected() ? keyboardMouse.Compose(slots[0].state) : PadInputState{};
        const auto state = handOff(0, composed);
        PadPublishInput_nid_postfix(state);
        if (auto* telemetry = VideoOutTelemetry::Instance(); telemetry != nullptr) VideoOutTelemetry::EmitPadIfChanged(*telemetry, telemetryFilter, frame, state);
    } else {
        PadPublishInputForPort_nid_postfix(slot, handOff(slot, slots[slot].state));
    }
}
