# nps5: controllers on arm64 macOS (DualSense, Switch Pro, Xbox, keyboard and mouse)

All controller input goes through SDL2's GameController API (`libSceVideoOut/src/PadInput.cpp`) and is published into
libScePad's state (`libScePad/src/PadState.cpp`); the guest reads it with `scePadReadState` / `scePadRead`. Output
(rumble, light bar, adaptive triggers, motion on/off) flows the other way: `scePadSetVibration`,
`scePadSetLightBar`, `scePadSetTriggerEffect`, ... store a `PadOutputState`, `PadInput::applyOutput` sends it to SDL.

## SDL setup (`PadInput::ConfigureSdl`)

Called by `VideoOutDriver` before `SDL_InitSubSystem` (the hints are read when the joystick subsystem starts) and again
by `PadInput`. It uses `SDL_HINT_DEFAULT` priority, so an `SDL_*` environment variable still wins
(`SDL_JOYSTICK_HIDAPI_SWITCH=0` turns a driver off).

| hint | value | why |
|---|---|---|
| `SDL_JOYSTICK_HIDAPI`, `_PS5`, `_PS5_RUMBLE`, `_PS5_PLAYER_LED`, `_PS4` | 1 | DualSense over HIDAPI: touchpad, gyro/accel, light bar, player LEDs, adaptive-trigger output reports, rumble |
| `SDL_JOYSTICK_HIDAPI_SWITCH` | 1 | Switch Pro: full button set, HD-rumble motors, gyro/accel, player LED |
| `SDL_JOYSTICK_HIDAPI_XBOX` | 1 | Xbox One/Series (rumble, trigger rumble) |
| `SDL_GAMECONTROLLER_USE_BUTTON_LABELS` | **0** | SDL's default (1) swaps A/B and X/Y on Nintendo pads so the *label* matches; with 0 `SDL_CONTROLLER_BUTTON_A` is always the south button |
| `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS` | 1 | input keeps flowing while the window is not focused |

Audit result (before this change): only `PS5_RUMBLE` was set, after SDL had already started in `VideoOutDriver`; the
button-label hint was left at SDL's default, which would have swapped Cross/Circle and Square/Triangle on a Switch Pro.
Capture was ignored, and a virtual pad could not be told apart from a real family.

## Mapping (positional, `PadInput::sampleController`)

| SDL button (position) | PS5 pad | notes |
|---|---|---|
| A (south), B (east), X (west), Y (north) | Cross, Circle, Square, Triangle | identical on every pad type, Nintendo labels do not matter |
| LEFTSHOULDER / RIGHTSHOULDER | L1 / R1 | |
| TRIGGERLEFT / TRIGGERRIGHT | L2 / R2 analog 0..255, digital bit when > 0 | Switch Pro ZL/ZR are digital: 0 or 255 |
| LEFTSTICK / RIGHTSTICK | L3 / R3 | |
| START | Options | Switch +, Xbox Menu, DualSense Options |
| DPAD_* | Up/Right/Down/Left | |
| TOUCHPAD (PS4/PS5) | TouchPad click | |
| BACK (Xbox View, Switch minus) and MISC1 on a Nintendo pad (Capture) | TouchPad click at the pad's centre, finger 0 active | pads without a touchpad |
| GUIDE (PS button, Xbox, Switch Home) | not delivered | the PS button belongs to the console OS; scePad never reports it |
| MISC1 on DualSense (mic mute), paddles | not delivered | |

* Sticks: 16-bit SDL axis to 0..255, radial deadzone per stick (`LeftStickDeadzone`, `RightStickDeadzone`, percent, set in the mapping file).
* Touchpad: up to two fingers from `SDL_GameControllerGetTouchpadFinger`, scaled to 1920x1080 (DualSense only).
* Motion: `SDL_GameControllerSetSensorEnabled` on accel and gyro when the title leaves motion on (the default);
  DualSense and Switch Pro both provide them. The values go to `PadData.acceleration_*` / `angular_velocity_*`.
* Several pads: up to 4. Slot 0 is user 0 and is composed with keyboard and mouse; slot N is player N+1
  (`PadSetPortConnected`, `UserServiceSetUserConnected`). Hotplug uses `SDL_CONTROLLERDEVICEADDED/REMOVED`; a removed pad frees its slot.
* Connection type is always `LOCAL` (what the console reports for a paired pad, wired or Bluetooth).

## Output

| call | DualSense | Switch Pro | Xbox / MFi |
|---|---|---|---|
| `scePadSetVibration` | `SDL_GameControllerRumble` (large/small motor, refreshed every 700 ms, scaled by `ANYPS5_RUMBLE`) | rumble (HD-rumble motors) | rumble |
| `scePadSetVibrationMode` | recorded per port (`Pad::SetVibrationMode`); the SDL rumble call does not change | same | same |
| `scePadSetLightBar` | `SDL_GameControllerSetLED` | no LED, ignored | ignored |
| player LEDs | SDL drives the player-number LEDs (`PS5_PLAYER_LED`) | SDL drives the player LED | none |
| `scePadSetTriggerEffect` | `SDL_GameControllerSendEffect` with the DualSense effect block, see below | not available: the call succeeds, nothing is sent | `SDL_GameControllerRumbleTriggers` from the effect's fallback strength (Xbox One/Series only) |
| `scePadGetTriggerEffectState` | state read back from the pad's input report (`DualSenseStatus`) | reports "no effect" | reports "no effect" |
| `scePadIsRemoteController` | false | false | false |

Adaptive-trigger report (`libScePad/include/PadTriggerEffect.hpp`, 47 bytes, passed to `SDL_GameControllerSendEffect`
which merges it into the DualSense output report 0x02): byte 0 is the valid-flag byte (`0x04` right trigger,
`0x08` left trigger), the right trigger's 11 effect bytes start at offset 10, the left trigger's at offset 21
(mode byte first, then the mode's parameters; mode 0x05 switches the effect off). The layout is
`SDL_DS5EffectsState_t` of SDL's HIDAPI PS5 driver: `ucEnableBits1, ucEnableBits2, ucRumbleRight, ucRumbleLeft,
ucHeadphoneVolume, ucSpeakerVolume, ucMicrophoneVolume, ucAudioEnableBits, ucMicLightMode, ucAudioMuteBits,
rgucRightTriggerEffect[11], rgucLeftTriggerEffect[11], ...`. (SDL's source is not installed here; the offsets are the
ones the existing code already uses and that a real DualSense accepts.)

## Keyboard and mouse

Built-in bindings (no file needed): Return/Space Cross, C Circle, I Triangle, left mouse Square, Q/E L1/R1,
R / mouse 4 and right mouse L2/R2, Shift/Ctrl L3/R3, Escape Options, arrows (and the wheel) D-pad, W/A/S/D left stick,
T/F/G/H right stick, Backspace/Tab touchpad left/right, X shake, middle mouse or F7 mouse-to-stick,
F9 pad status, F10 overlay, F11 fullscreen, F12 screenshot, F1 reload mapping. Full table: `docs/user/INPUT_MAPPING.md`.

Configuration, layered in this order (later layers replace what they name, the rest keeps its default):
1. `~/Library/Application Support/nativeps5/input.json` (or the file named by `NPS5_INPUT_JSON`)
2. `anyps5-input.ini` beside the executable (or `ANYPS5_INPUT_CONFIG`), the existing format
3. `anyps5-input.<TITLEID>.ini` for the running title

```json
{
  "bindings": {
    "Cross":    ["KEY:J", "KEY:Space"],
    "Circle":   "KEY:K",
    "L2":       ["MOUSE:Right"],
    "ToggleFullscreen": "KEY:F11"
  },
  "settings": { "LeftStickDeadzone": 12, "MouseSensitivity": 1.5, "MouseStickTarget": "Right" }
}
```

Action names, `KEY:`/`MOUSE:`/`WHEEL:`/`PAD:` sources and setting names are those of the INI format (the JSON is converted
to INI text and parsed by the same code, so errors name the file and the action). The first value of an action replaces
its defaults, further values add alternatives. A flat object (`{"Cross": "KEY:J"}`) is accepted too. F1 (`ReloadInputMapping`) re-reads
all layers; a file with an error leaves the previous mapping active and logs why.

## Debugging and tests without hardware

* `NPS5_PAD_LOG=1` prints every pad state change (buttons, sticks in steps of 8, triggers, touch, gyro) to stderr as
  `[pad] slot N (name): buttons=0x... L=(x,y) R=(x,y) L2=.. R2=..`, and each scripted event.
* `NPS5_VPAD_SCRIPT` attaches one SDL virtual game controller driven by a script (grammar in
  `libSceVideoOut/include/VirtualPadScript.hpp`), e.g. `f90:tap=up;f150:tap=up;f210:tap=left` (`f<N>`: N frames after the first
  update; plain numbers are milliseconds; `+btn`, `-btn`, `tap=btn`, `lx=0.5`, `rt=1`). `@path` reads the script from a file.
  `NPS5_VPAD_TYPE=ps5|switch|xbox` gives the virtual pad that family's vendor/product id, so family-specific handling
  (Switch Capture, trigger rumble, ...) is exercised too.
* Host test, no hardware, no window: `cmake --build build-arm64 --target nps5_pad_host_tests` then run
  `build-arm64/core/libs/prx/libSceVideoOut/nps5_pad_host_tests`. It attaches virtual joysticks (generic, Xbox, DualSense,
  Switch Pro identities), feeds buttons/axes and asserts `Pad::ReadState`, including the positional mapping on the
  Switch identity, Capture and View, analog triggers, stick range and deadzone, the SDL hints, the script parser, the scripted
  pad and the JSON mapping. SDL 2.32's virtual joystick has no touchpad/sensor feed, so those are covered by the manual
  test in `work/CONTROLLER-TEST.md`.
* Build note: `libnps5_gpu.dylib` is linked with `-force_load` of the static libraries, which ninja does not track as
  inputs; after changing Pad/VideoOut sources delete `core/libs/libs/unpatched/libnps5_gpu.dylib` before building `nps5_gpu`.
* macOS may ask for Input Monitoring permission the first time HIDAPI opens a DualSense or Switch Pro.
