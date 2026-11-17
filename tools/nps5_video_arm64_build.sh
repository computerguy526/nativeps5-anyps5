#!/bin/zsh
# nps5: build libSceVideoOut / libScePad / libSceUserService (+ Mouse/Keyboard/Ime deps)
# as arm64 static libs without the top-level CMake (which forces x86_64 on macOS).
# usage: tools/nps5_video_arm64_build.sh [build-dir]   (default build-video)
# env: AGC_GEN = dir containing prx/libSceAgcDriver/generated and prx/libc/run_report_generated
#      (any configured AnyPS5 build dir's core/libs parent), default: ./build/core/libs
set -e
W=$(cd "$(dirname "$0")/.." && pwd); B=${1:-$W/build-video}; G=${AGC_GEN:-$W/build/core/libs}
P=core/libs/prx
INC=(-I$W/core/libs -I$W/3rdparty/SDL2/include -I$W/3rdparty/Vulkan-Headers/include -I$G/prx/libSceAgcDriver/generated -I$G/prx/libc/run_report_generated -I$W/core/shader/recompiler -I$W/core/Decoder/Png/include)
mkdir -p $B/obj
cc1(){ /usr/bin/c++ -DAPS5_ENABLE_TIMING_LOG=0 $INC -O1 -DNDEBUG -std=c++20 -arch arm64 -fPIC -fexperimental-library -c $W/$1 -o $B/obj/$(echo ${1#core/libs/prx/} | tr / _).o; }
U=($P/libSceUserService/{Export,InventoryExports,Inventory1360Exports,LegacyExports}.cpp)
D=($P/libScePad/{Export,InventoryExports,src/PadState,src/HostVolumeState}.cpp)
V=($P/libSceVideoOut/{Unimplemented,InventoryExports}.cpp $P/libSceVideoOut/src/{Event,Buffer,Output,VideoOutDriver,DisplayWindow,HostFullscreen,PadInput,PadStatusSdl,Telemetry,MouseInput,KeyboardInput,ImeTextInput,DialogInput,InputMapping,KeyboardMouse,HostHdrProbe}.cpp $P/libkernel/Time/TimedWait.cpp)
X=($P/libSceMouse/{InventoryExports,Export,src/mouse_impl}.cpp $P/libSceKeyboard/{Export,PadEmulate,src/keyboard_impl}.cpp $P/libSceIme/{Export,src/ModalDialog,src/PsnDialogNotice}.cpp)
for f in $U $D $V $X; do cc1 $f; done
o(){ for f in "$@"; do echo $B/obj/$(echo ${f#core/libs/prx/} | tr / _).o; done; }
libtool -static -o $B/libSceUserService.a $(o $U)
libtool -static -o $B/libScePad.a $(o $D)
libtool -static -o $B/libSceVideoOut.a $(o $V)
libtool -static -o $B/libSceVideoOutDeps.a $(o $X)
ls -la $B/*.a
