// nps5: host test for the SDL controller path on arm64 macOS, without hardware.
//
// SDL virtual joysticks (SDL_JoystickAttachVirtualEx) stand in for a generic pad, a DualSense, a Switch Pro and an Xbox
// pad. The test feeds button and axis states, runs PadInput::Update and asserts what scePadReadState reports. It also
// covers the SDL hints, the scripted pad (NPS5_VPAD_SCRIPT) and the input.json keyboard/mouse mapping.
//
// SDL's virtual joystick API has no touchpad or sensor feed in SDL 2.32, so touchpad coordinates and gyro are covered
// by docs/nps5-controllers.md's manual test only.
#include "SDL.h"
#include "SDL_gamecontroller.h"
#include "SDL_joystick.h"
#include "prx/libSceVideoOut/include/PadInput.hpp"
#include "prx/libSceVideoOut/include/DisplayWindow.hpp"
#include "prx/libSceVideoOut/include/VirtualPadScript.hpp"
#include "prx/libScePad/include/InputMapping.hpp"
#include "prx/libScePad/include/PadState.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#undef main

namespace {

void Require(bool condition, std::source_location where = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "nps5_pad_host: check failed at line %u\n", static_cast<unsigned>(where.line()));
        std::abort();
    }
}

constexpr std::uint32_t Cross = 0x4000, Circle = 0x2000, Square = 0x8000, Triangle = 0x1000;
constexpr std::uint32_t L1 = 0x0400, R1 = 0x0800, L2 = 0x0100, R2 = 0x0200, L3 = 0x0002, R3 = 0x0004;
constexpr std::uint32_t Options = 0x0008, Up = 0x0010, Right = 0x0020, Down = 0x0040, Left = 0x0080, TouchPad = 0x100000;

struct Identity {
    const char* name;
    std::uint16_t vendor, product;
};

// Positional mapping: the virtual pad exposes SDL's standard layout, so SDL button A is the south button whatever the
// maker prints on it. A Nintendo identity must not swap A/B or X/Y.
void StandardLayout(const Identity& identity, bool nintendo, bool sony) {
    SDL_VirtualJoystickDesc desc{};
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = 6;
    desc.nbuttons = 16;
    desc.vendor_id = identity.vendor;
    desc.product_id = identity.product;
    desc.name = identity.name;
    const int device = SDL_JoystickAttachVirtualEx(&desc);
    Require(device >= 0);
    Require(SDL_IsGameController(device));

    {
        DisplayWindow window;
        PadInput padInput;
        Require(padInput.SlotOccupied(0));
        Pad::Initialize();
        SDL_Joystick* joystick = SDL_JoystickFromInstanceID(SDL_JoystickGetDeviceInstanceID(device));
        Require(joystick != nullptr);
        SDL_GameController* controller = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(device));
        Require(controller != nullptr);
        // SDL itself reports SDL_CONTROLLER_TYPE_VIRTUAL; PadInput maps the declared vendor/product to the real family.
        Require(SDL_GameControllerGetType(controller) == SDL_CONTROLLER_TYPE_VIRTUAL);
        std::printf("nps5_pad_host: %s (vendor %04x product %04x, treated as %s)\n", identity.name, identity.vendor, identity.product, nintendo ? "Nintendo" : sony ? "Sony" : "generic");

        Require(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_JOYSTICK_AXIS_MIN) == 0);
        Require(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, SDL_JOYSTICK_AXIS_MIN) == 0);
        padInput.Update(window);
        const PadData idle = Pad::ReadState(0);
        Require(idle.connected && idle.buttons == 0);
        Require(idle.left_stick_x == 128 && idle.left_stick_y == 128 && idle.right_stick_x == 128 && idle.right_stick_y == 128);
        Require(idle.analog_buttons_l2 == 0 && idle.analog_buttons_r2 == 0);

        const auto press = [&](SDL_GameControllerButton button, std::uint32_t expected) {
            Require(SDL_JoystickSetVirtualButton(joystick, button, 1) == 0);
            padInput.Update(window);
            const PadData down = Pad::ReadState(0);
            Require((down.buttons & expected) == expected);
            Require(SDL_JoystickSetVirtualButton(joystick, button, 0) == 0);
            padInput.Update(window);
            Require((Pad::ReadState(0).buttons & expected) == 0);
        };
        // South / east / west / north face buttons are Cross / Circle / Square / Triangle.
        press(SDL_CONTROLLER_BUTTON_A, Cross);
        press(SDL_CONTROLLER_BUTTON_B, Circle);
        press(SDL_CONTROLLER_BUTTON_X, Square);
        press(SDL_CONTROLLER_BUTTON_Y, Triangle);
        press(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, L1);
        press(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, R1);
        press(SDL_CONTROLLER_BUTTON_LEFTSTICK, L3);
        press(SDL_CONTROLLER_BUTTON_RIGHTSTICK, R3);
        press(SDL_CONTROLLER_BUTTON_START, Options);
        press(SDL_CONTROLLER_BUTTON_DPAD_UP, Up);
        press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, Right);
        press(SDL_CONTROLLER_BUTTON_DPAD_DOWN, Down);
        press(SDL_CONTROLLER_BUTTON_DPAD_LEFT, Left);

        // Only one face button at a time reaches the pad (no crosstalk between A/B/X/Y).
        Require(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, 1) == 0);
        padInput.Update(window);
        Require((Pad::ReadState(0).buttons & (Cross | Circle | Square | Triangle)) == Cross);
        Require(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, 0) == 0);

        // Analog triggers: released, half, full. The digital L2/R2 bits follow the analog value.
        Require(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 0) == 0);
        Require(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, SDL_JOYSTICK_AXIS_MAX) == 0);
        padInput.Update(window);
        const PadData triggers = Pad::ReadState(0);
        Require(triggers.analog_buttons_l2 > 100 && triggers.analog_buttons_l2 < 156);
        Require(triggers.analog_buttons_r2 == 255);
        Require((triggers.buttons & (L2 | R2)) == (L2 | R2));
        Require(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_JOYSTICK_AXIS_MIN) == 0);
        Require(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, SDL_JOYSTICK_AXIS_MIN) == 0);

        // Sticks: full deflection reaches the ends, centre stays at 128, a tiny drift is inside the deadzone.
        Require(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX) == 0);
        Require(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN) == 0);
        Require(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTX, 600) == 0);
        padInput.Update(window);
        const PadData sticks = Pad::ReadState(0);
        Require(sticks.left_stick_x == 255 && sticks.left_stick_y == 0);
        Require(sticks.right_stick_x >= 126 && sticks.right_stick_x <= 130);
        Require(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 0) == 0);
        Require(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTY, 0) == 0);
        Require(SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTX, 0) == 0);

        // View/Back: touchpad click on every pad but a Sony one (which has a real touchpad button).
        Require(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_BACK, 1) == 0);
        padInput.Update(window);
        const PadData back = Pad::ReadState(0);
        Require(sony ? (back.buttons & TouchPad) == 0 : ((back.buttons & TouchPad) != 0 && back.touch_data_touch_num == 1));
        Require(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_BACK, 0) == 0);
        // Capture (Switch) is SDL's MISC1: a touchpad click on a Nintendo pad only.
        Require(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_MISC1, 1) == 0);
        padInput.Update(window);
        Require(((Pad::ReadState(0).buttons & TouchPad) != 0) == nintendo);
        Require(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_MISC1, 0) == 0);
        padInput.Update(window);
        Require((Pad::ReadState(0).buttons & TouchPad) == 0);
    }
    Require(SDL_JoystickDetachVirtual(device) == 0);
}

void ScriptedPad() {
    Require(setenv("NPS5_VPAD_TYPE", "switch", 1) == 0);
    Require(setenv("NPS5_VPAD_SCRIPT", "0:+a;80:-a;80:ly=-1;160:tap=circle;f9999:+x", 1) == 0);
    {
        DisplayWindow window;
        PadInput padInput;
        Require(padInput.SlotOccupied(0));
        Pad::Initialize();
        bool sawCross = false, sawUp = false, sawCircle = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline && !(sawCross && sawUp && sawCircle)) {
            padInput.Update(window);
            const PadData data = Pad::ReadState(0);
            sawCross |= (data.buttons & Cross) != 0;
            sawUp |= data.left_stick_y == 0;
            sawCircle |= (data.buttons & Circle) != 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Require(sawCross && sawUp && sawCircle);
        Require((Pad::ReadState(0).buttons & Square) == 0);  // the f9999 event never fires
    }
    unsetenv("NPS5_VPAD_SCRIPT");
    unsetenv("NPS5_VPAD_TYPE");
}

void ScriptParser() {
    std::vector<Pad::VirtualPadEvent> events;
    std::string error;
    Require(Pad::ParseVirtualPadScript("100:+cross; 50:ly=-1\n200:tap=south;f10:-capture;300:rt=0.5", events, error));
    Require(events.size() == 6);
    Require(events[0].when == 50 && events[0].isAxis && events[0].index == 1 && events[0].value == -32767);
    Require(events[1].when == 100 && events[1].index == 0 && events[1].value == 1);
    Require(events[2].when == 200 && events[2].value == 1);
    Require(events[3].when == 300 && events[3].isAxis && events[3].index == 5 && events[3].value > -100 && events[3].value < 100);
    Require(events[4].when == 320 && events[4].value == 0);
    Require(events[5].byFrame && events[5].when == 10 && events[5].index == 15);
    for (const char* bad : {"x", "10:", "10:+nothing", "10:lx=2", "10:frob", "-5:+a", "10:tap=zzz"}) Require(!Pad::ParseVirtualPadScript(bad, events, error));
}

bool Throws(const std::string& json) {
    Pad::InputConfiguration configuration;
    try { Pad::ApplyInputMappingJson(configuration, json, "test.json"); } catch (const std::runtime_error&) { return true; }
    return false;
}

bool HasBinding(const Pad::InputConfiguration& configuration, std::uint32_t button, SDL_Scancode key) {
    for (const auto& binding : configuration.bindings) {
        if (binding.control == Pad::InputControl::Button && static_cast<std::uint32_t>(binding.button) == button && binding.key == key) return true;
    }
    return false;
}

void JsonMapping() {
    Pad::InputConfiguration configuration = Pad::LoadInputMapping();
    Require(HasBinding(configuration, Cross, SDL_SCANCODE_RETURN) && HasBinding(configuration, Cross, SDL_SCANCODE_SPACE));
    Pad::ApplyInputMappingJson(configuration,
        R"({"bindings": {"Cross": ["KEY:J", "KEY:K"], "Circle": "KEY:L", "LeftStickUp": "KEY:Up"}, "settings": {"LeftStickDeadzone": 12, "MouseStickTarget": "Right"}})", "test.json");
    Require(HasBinding(configuration, Cross, SDL_SCANCODE_J) && HasBinding(configuration, Cross, SDL_SCANCODE_K));
    Require(!HasBinding(configuration, Cross, SDL_SCANCODE_RETURN) && !HasBinding(configuration, Cross, SDL_SCANCODE_SPACE));
    Require(HasBinding(configuration, Circle, SDL_SCANCODE_L) && !HasBinding(configuration, Circle, SDL_SCANCODE_C));
    Require(HasBinding(configuration, Triangle, SDL_SCANCODE_I));  // untouched actions keep their defaults
    Require(configuration.settings.leftStickDeadzonePercent == 12 && configuration.settings.mouseControlsRightStick);
    Require(Throws(R"({"bindings": {"Cross": "KEY:NoSuchKey"}})"));
    Require(Throws(R"({"bindings": {"Bogus": "KEY:A"}})"));
    Require(Throws(R"({"bindings": {"Cross": )"));
    Require(Throws(R"({"settings": {"LeftStickDeadzone": 500}})"));
    Require(Throws("not json"));

    // The file the loader reads: NPS5_INPUT_JSON, else ~/Library/Application Support/nativeps5/input.json.
    const auto path = std::filesystem::temp_directory_path() / ("nps5-input-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    std::ofstream(path) << R"({"Square": "KEY:M", "Triangle": ["KEY:N"]})";
    Require(setenv("NPS5_INPUT_JSON", path.c_str(), 1) == 0);
    Require(Pad::InputJsonPath() == path);
    const auto loaded = Pad::LoadInputMapping();
    Require(HasBinding(loaded, Square, SDL_SCANCODE_M) && HasBinding(loaded, Triangle, SDL_SCANCODE_N) && !HasBinding(loaded, Triangle, SDL_SCANCODE_I));
    std::ofstream(path) << "{ broken";
    Pad::InputConfiguration rejected;
    std::string error;
    Require(!Pad::TryLoadInputMapping({}, rejected, error) && error.find("input mapping") != std::string::npos);
    unsetenv("NPS5_INPUT_JSON");
    std::filesystem::remove(path);
    unsetenv("NPS5_INPUT_JSON");
    Require(Pad::InputJsonPath().filename() == "input.json" && Pad::InputJsonPath().string().find("Application Support/nativeps5") != std::string::npos);
}

}

int main() {
    ScriptParser();
    JsonMapping();

    // Hints: set before SDL_Init, default priority (the environment wins), positional face buttons.
    PadInput::ConfigureSdl();
    for (const char* name : {SDL_HINT_JOYSTICK_HIDAPI_PS5, SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, SDL_HINT_JOYSTICK_HIDAPI_SWITCH, SDL_HINT_JOYSTICK_HIDAPI_XBOX}) {
        const char* value = SDL_GetHint(name);
        Require(value != nullptr && std::string(value) == "1");
    }
    Require(std::string(SDL_GetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS)) == "0");
    // Keep real hardware out of the pad slots this test checks (override the hints we just verified).
    for (const char* name : {SDL_HINT_JOYSTICK_HIDAPI, SDL_HINT_JOYSTICK_HIDAPI_PS5, SDL_HINT_JOYSTICK_HIDAPI_PS4, SDL_HINT_JOYSTICK_HIDAPI_SWITCH, SDL_HINT_JOYSTICK_HIDAPI_XBOX}) {
        SDL_SetHintWithPriority(name, "0", SDL_HINT_OVERRIDE);
    }
    Require(SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) == 0);
    if (SDL_NumJoysticks() > 0) {
        std::fprintf(stderr, "nps5_pad_host: skipped, SDL sees %d host joysticks\n", SDL_NumJoysticks());
        return 77;
    }

    StandardLayout({"Virtual Generic Pad", 0, 0}, false, false);
    StandardLayout({"Virtual Xbox Pad", 0x045e, 0x0b12}, false, false);
    StandardLayout({"Virtual DualSense", 0x054c, 0x0ce6}, false, true);
    StandardLayout({"Virtual Switch Pro", 0x057e, 0x2009}, true, false);
    ScriptedPad();

    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
    std::puts("nps5_pad_host: OK");
    return 0;
}
