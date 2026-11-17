#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_PADINPUT_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_PADINPUT_HPP

#include "SDL_events.h"
#include "SDL_gamecontroller.h"
#include "SDL_hidapi.h"
#include "prx/libScePad/include/InputMapping.hpp"
#include "prx/libScePad/include/KeyboardMouse.hpp"
#include "prx/libScePad/include/PadState.hpp"
#include "prx/libScePad/include/Pad.hpp"
#include "prx/libScePad/include/PadIdentify.hpp"
#include "prx/libSceVideoOut/include/PadOutputPacing.hpp"
#include "prx/libSceVideoOut/include/Port0Presence.hpp"
#include "prx/libSceAgcDriver/Execution/include/PadStatus.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include "prx/libSceVideoOut/include/Telemetry.hpp"
#include "prx/libSceVideoOut/include/VirtualPadScript.hpp"
#include <vector>

class DisplayWindow;

class PadInput {
public:
    PadInput();
    ~PadInput();
    PadInput(const PadInput&) = delete;
    PadInput& operator=(const PadInput&) = delete;
    // nps5: SDL hints the controller drivers read at SDL_Init (HIDAPI drivers for DualSense/Switch Pro/Xbox, positional
    // face buttons). Idempotent; VideoOutDriver calls it before SDL_InitSubSystem, PadInput again for hosts that init SDL
    // themselves. A hint already set in the environment (SDL_JOYSTICK_HIDAPI_SWITCH=0 ...) wins.
    static void ConfigureSdl();
    void HandleEvent(const SDL_Event& event, DisplayWindow& window);
    void Update(const DisplayWindow& window);
    bool MouseModeEnabled() const { return keyboardMouse.MouseMode(); }
    bool SlotOccupied(int slot) const {
        return slot >= 0 && slot < PAD_MAX_CONTROLLERS && slots[slot].controller != nullptr;
    }

private:
    struct ControllerSlot {
        SDL_GameController* controller = nullptr;
        SDL_JoystickID instanceId = -1;
        PadInputState state{};
        std::uint32_t outputSequence = 0;
        PadOutputState outputState{};
        bool outputPending = false;
        bool rumbleLive = false;
        std::chrono::steady_clock::time_point nextRumbleRefresh{};
        LightBarPacer lightBar;
        TriggerEffectCache triggerEffect;
        std::uint32_t heldThroughCapture = 0;
        SDL_hid_device* statusHid = nullptr;
        Pad::IdentifyScheduler identify;
        AgcDriver::PadKind kind = AgcDriver::PadKind::Unknown;
        std::string name;
        PadInputState logged{};
        bool loggedValid = false;
    };

    explicit PadInput(Pad::InputConfiguration configuration);
    void publish(std::uint64_t frame);
    void applyMappingSnapshot();
    void reloadInputMapping();
    void dispatchPadChords(DisplayWindow& window);
    void applyRequests(const Pad::InputRequests& requests, bool windowFocused, DisplayWindow& window);
    void startInputMappingWatch();
    void watchInputMapping(std::string titleId, Pad::InputMappingWatch watch);
    void publish(std::uint64_t frame, int slot);
    void setMouseMode(bool enabled);
    void openAvailableControllers();
    void openController(int deviceIndex);
    void closeController(int slot);
    PadInputState sampleController(int slot) const;
    PadInputState handOff(int slot, PadInputState state);
    void applyOutput();
    void enableSensors(int slot);
    void openStatus(int slot);
    void pollStatus(int slot);
    void closeStatus(int slot);
    AgcDriver::PadStatusSnapshot statusSnapshot() const;
    void reportStatus();
    void updatePort0();
    void applyPort0(bool connected);
    void openHaptics(SDL_GameController* controller);
    void closeHaptics();
    void streamHaptics();
    void attachScriptedPad();
    void advanceScriptedPad(const DisplayWindow& window);
    void logPadState(int slot);

    Pad::InputMappingStore mappings;
    std::shared_ptr<const Pad::InputConfiguration> appliedMapping;
    Pad::KeyboardMouse keyboardMouse;
    Pad::IdentifyMode identifyMode = Pad::IdentifyMode::Off;
    std::array<ControllerSlot, PAD_MAX_CONTROLLERS> slots{};
    VideoOutTelemetry::PadChangeFilter telemetryFilter;
    AgcDriver::PadStatusProvider statusProvider;
    Port0Presence::Mode port0Mode;
    Port0Presence::Presence port0;
    bool padCaptured = false;
    bool hostFocusSynced = false;
    SDL_hid_device* haptics = nullptr;
    std::atomic<bool> hapticsRunning{false};
    std::thread hapticsThread;
    bool hapticsRumbled = false;
    std::atomic<std::chrono::steady_clock::rep> hapticsReconfigureAt{0};
    bool padLog = false;
    SDL_Joystick* scriptedJoystick = nullptr;
    int scriptedDevice = -1;
    std::vector<Pad::VirtualPadEvent> script;
    std::size_t scriptNext = 0;
    bool scriptStarted = false;
    std::chrono::steady_clock::time_point scriptStart{};
    std::uint64_t scriptStartFrame = 0;
    struct TapStamp { std::uint64_t ms = 0; std::uint64_t reads = 0; };
    std::array<TapStamp, 64> tapStamps{};   // press time / guest-read count per scripted button index (one entry per tap, not one overwritten pair)
    bool tapCapLogged = false;
    void notePress(int index, std::uint64_t elapsedMs);
    bool tapHeldLongEnough(int index, std::uint64_t elapsedMs);
    std::thread scriptThread;            // NPS5_VPAD_ASYNC=1: millisecond script events applied from their own thread
    std::atomic<bool> scriptStop{false};
    std::thread mappingWatchThread;
    std::atomic<bool> mappingWatchStop{false};
};

#endif
