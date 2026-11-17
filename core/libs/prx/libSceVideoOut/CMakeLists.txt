add_library(libSceVideoOut SHARED EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_SOURCE_DIR}/Unimplemented.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/InventoryExports.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Event.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Buffer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Output.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/VideoOutDriver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/DisplayWindow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/HostFullscreen.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/PadInput.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/PadStatusSdl.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Telemetry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MouseInput.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/KeyboardInput.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ImeTextInput.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/DialogInput.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/InputMapping.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/KeyboardMouse.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/HostHdrProbe.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/DisplayLinkBeat.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../libkernel/Time/TimedWait.cpp
)

target_include_directories(libSceVideoOut PRIVATE
        ${LIBS_INCLUDE_DIR}
        ${ANYPS5_SDL2_INCLUDE_DIR}
        ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
        ${CMAKE_BINARY_DIR}/core/libs/prx/libSceAgcDriver/generated
)
add_dependencies(libSceVideoOut agc_trace_revision)
if(APPLE)
    set(videoOutSdl libSceAgcDriver)
    set(videoOutSdlInclude ${ANYPS5_SDL2_INCLUDE_DIR})
else()
    set(videoOutSdl SDL2-static)
    set(videoOutSdlInclude)
endif()
target_link_libraries(libSceVideoOut PRIVATE libSceAgcDriver libScePad libSceUserService libSceMouse libSceKeyboard libSceIme libc libkernel decoder_png ${videoOutSdl})
if(WIN32)
    target_link_libraries(libSceVideoOut PRIVATE comctl32 dxgi)
endif()
set_target_properties(libSceVideoOut PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(libSceVideoOut)

foreach(runtimeLinkFlag IN ITEMS -static-libgcc -static-libstdc++ -static)
    string(REPLACE "${runtimeLinkFlag}" "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
endforeach()
add_test_executable(video_out_mouse_tests EXCLUDE_FROM_ALL tests/MouseInput.cpp)
target_include_directories(video_out_mouse_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_SDL2_INCLUDE_DIR})
target_link_libraries(video_out_mouse_tests PRIVATE libSceVideoOut libSceMouse libSceIme libc ${videoOutSdl})
set_target_properties(video_out_mouse_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(video_out_mouse_tests)
if(BUILD_TESTING)
    set_target_properties(video_out_mouse_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME video_out_mouse COMMAND video_out_mouse_tests)
    if(WIN32)
        set_tests_properties(video_out_mouse PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceVideoOut>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceMouse>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceIme>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(video_out_dialog_input_tests EXCLUDE_FROM_ALL tests/DialogInput.cpp)
target_include_directories(video_out_dialog_input_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_SDL2_INCLUDE_DIR})
target_link_libraries(video_out_dialog_input_tests PRIVATE libSceVideoOut libSceIme libc ${videoOutSdl})
set_target_properties(video_out_dialog_input_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(video_out_dialog_input_tests)
if(BUILD_TESTING)
    set_target_properties(video_out_dialog_input_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME video_out_dialog_input COMMAND video_out_dialog_input_tests)
    if(WIN32)
        set_tests_properties(video_out_dialog_input PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceVideoOut>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceIme>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

add_test_executable(video_out_flip_tests EXCLUDE_FROM_ALL tests/Flip.cpp)
target_include_directories(video_out_flip_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_SDL2_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/core/shader/recompiler ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
target_link_libraries(video_out_flip_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc ${videoOutSdl})
set_target_properties(video_out_flip_tests PROPERTIES CXX_EXTENSIONS OFF)
configure_windows_unwind(video_out_flip_tests)
if(BUILD_TESTING)
    add_test_executable(video_out_host_fullscreen_tests tests/HostFullscreen.cpp)
    target_include_directories(video_out_host_fullscreen_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_SDL2_INCLUDE_DIR})
    if(APPLE)
        target_link_libraries(video_out_host_fullscreen_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc)
    else()
        target_link_libraries(video_out_host_fullscreen_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc SDL2-static)
    endif()
    configure_windows_unwind(video_out_host_fullscreen_tests)
    add_test(NAME video_out_host_fullscreen COMMAND video_out_host_fullscreen_tests)
    set_tests_properties(video_out_host_fullscreen PROPERTIES TIMEOUT 20)
    if(WIN32)
        set_tests_properties(video_out_host_fullscreen PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(video_out_output_mode_policy_tests tests/OutputModePolicy.cpp)
    target_include_directories(video_out_output_mode_policy_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_SDL2_INCLUDE_DIR})
    target_link_libraries(video_out_output_mode_policy_tests PRIVATE libSceVideoOut libc libkernel)
    configure_windows_unwind(video_out_output_mode_policy_tests)
    add_test(NAME video_out_output_mode_policy COMMAND video_out_output_mode_policy_tests)
    set_tests_properties(video_out_output_mode_policy PROPERTIES TIMEOUT 20)
    if(WIN32)
        set_tests_properties(video_out_output_mode_policy PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    add_test_executable(guest_video_out_output_mode_checks_tests ${LIBS_INCLUDE_DIR}/tests/GuestVideoOutOutputModeChecks.cpp)
    target_include_directories(guest_video_out_output_mode_checks_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_video_out_output_mode_checks_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc)
    configure_windows_unwind(guest_video_out_output_mode_checks_tests)
    add_test(NAME guest_video_out_output_mode_checks COMMAND guest_video_out_output_mode_checks_tests)
    set_tests_properties(guest_video_out_output_mode_checks PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    if(WIN32)
        set_tests_properties(guest_video_out_output_mode_checks PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    add_test_executable(guest_video_out_firmware_answers_tests ${LIBS_INCLUDE_DIR}/tests/GuestVideoOutFirmwareAnswers.cpp)
    target_include_directories(guest_video_out_firmware_answers_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_video_out_firmware_answers_tests PRIVATE libSceVideoOut libkernel libc)
    configure_windows_unwind(guest_video_out_firmware_answers_tests)
    add_test(NAME guest_video_out_firmware_answers COMMAND guest_video_out_firmware_answers_tests)
    set_tests_properties(guest_video_out_firmware_answers PROPERTIES TIMEOUT 20)
    if(WIN32)
        set_tests_properties(guest_video_out_firmware_answers PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    add_test_executable(guest_video_out_high_frame_rate_tests ${LIBS_INCLUDE_DIR}/tests/GuestVideoOutHighFrameRate.cpp)
    target_include_directories(guest_video_out_high_frame_rate_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_video_out_high_frame_rate_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc)
    configure_windows_unwind(guest_video_out_high_frame_rate_tests)
    add_test(NAME guest_video_out_high_frame_rate COMMAND guest_video_out_high_frame_rate_tests)
    set_tests_properties(guest_video_out_high_frame_rate PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    if(WIN32)
        set_tests_properties(guest_video_out_high_frame_rate PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(guest_video_out_device_capability_tests ${LIBS_INCLUDE_DIR}/tests/GuestVideoOutDeviceCapability.cpp)
    target_include_directories(guest_video_out_device_capability_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_video_out_device_capability_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc)
    configure_windows_unwind(guest_video_out_device_capability_tests)
    add_test(NAME guest_video_out_device_capability COMMAND guest_video_out_device_capability_tests)
    set_tests_properties(guest_video_out_device_capability PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    if(WIN32)
        set_tests_properties(guest_video_out_device_capability PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(guest_video_out_current_output_mode_tests ${LIBS_INCLUDE_DIR}/tests/GuestVideoOutCurrentOutputMode.cpp)
    target_include_directories(guest_video_out_current_output_mode_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_video_out_current_output_mode_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc)
    configure_windows_unwind(guest_video_out_current_output_mode_tests)
    add_test(NAME guest_video_out_current_output_mode COMMAND guest_video_out_current_output_mode_tests)
    set_tests_properties(guest_video_out_current_output_mode PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    if(WIN32)
        set_tests_properties(guest_video_out_current_output_mode PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    if(WIN32)
        set(videoOutTestDriver "windows")
    else()
        set(videoOutTestDriver "offscreen")
    endif()
    set(ANYPS5_VIDEO_OUT_TEST_DRIVER "${videoOutTestDriver}" CACHE STRING
        "SDL video driver for VideoOut flip tests")
    add_test(NAME video_out_host_fullscreen_window COMMAND video_out_host_fullscreen_tests window)
    set_tests_properties(video_out_host_fullscreen_window PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0 ENVIRONMENT "SDL_VIDEODRIVER=${ANYPS5_VIDEO_OUT_TEST_DRIVER}")
    add_test(NAME video_out_host_fullscreen_failed_create COMMAND video_out_host_fullscreen_tests failedcreate)
    set_tests_properties(video_out_host_fullscreen_failed_create PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0 ENVIRONMENT "SDL_VIDEODRIVER=${ANYPS5_VIDEO_OUT_TEST_DRIVER}")
    if(WIN32)
        set_tests_properties(video_out_host_fullscreen_window video_out_host_fullscreen_failed_create PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    foreach(flipMode IN ITEMS labels labels_main)
        add_test(NAME video_out_flip_${flipMode} COMMAND video_out_flip_tests ${flipMode})
        set_tests_properties(video_out_flip_${flipMode} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0 ENVIRONMENT "APS5_NO_WGI=1;SDL_VIDEODRIVER=${ANYPS5_VIDEO_OUT_TEST_DRIVER}")
        if(WIN32)
            set_tests_properties(video_out_flip_${flipMode} PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()

    set_target_properties(video_out_flip_tests PROPERTIES EXCLUDE_FROM_ALL OFF)
    foreach(flipMode IN ITEMS lifetime reopen decode controls overlay padstatus unopened ps4 resolution resolution_override fresh
            present backtoback dcc pacing aftergpu onedevice deferred unregistered)
        if(flipMode STREQUAL "lifetime")
            add_test(NAME video_out_flip_${flipMode} COMMAND video_out_flip_tests)
        else()
            add_test(NAME video_out_flip_${flipMode} COMMAND video_out_flip_tests ${flipMode})
        endif()
        set_tests_properties(video_out_flip_${flipMode} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0 ENVIRONMENT "APS5_NO_WGI=1;SDL_VIDEODRIVER=${ANYPS5_VIDEO_OUT_TEST_DRIVER}")
        if(WIN32)
            set_tests_properties(video_out_flip_${flipMode} PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    foreach(flipMode IN ITEMS eop eop_main eop_aftergpu cpuflip)
        add_test(NAME video_out_flip_${flipMode} COMMAND video_out_flip_tests ${flipMode})
        set_tests_properties(video_out_flip_${flipMode} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0 ENVIRONMENT "APS5_NO_WGI=1;SDL_VIDEODRIVER=${ANYPS5_VIDEO_OUT_TEST_DRIVER}")
        if(WIN32)
            set_tests_properties(video_out_flip_${flipMode} PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    add_test(NAME video_out_flip_fps_limit COMMAND video_out_flip_tests fps_limit)
    set_tests_properties(video_out_flip_fps_limit PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0 ENVIRONMENT "APS5_NO_WGI=1;SDL_VIDEODRIVER=${ANYPS5_VIDEO_OUT_TEST_DRIVER};ANYPS5_FPS_LIMIT=20")
    if(WIN32)
        set_tests_properties(video_out_flip_fps_limit PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME video_out_flip_cpuflip_in_order COMMAND video_out_flip_tests cpuflip)
    set_tests_properties(video_out_flip_cpuflip_in_order PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0 ENVIRONMENT "APS5_NO_WGI=1;SDL_VIDEODRIVER=${ANYPS5_VIDEO_OUT_TEST_DRIVER};APS5_CPU_FLIP_IN_ORDER=1")
    if(WIN32)
        set_tests_properties(video_out_flip_cpuflip_in_order PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test(NAME video_out_flip_unavailable COMMAND video_out_flip_tests unavailable)
    set_tests_properties(video_out_flip_unavailable PROPERTIES TIMEOUT 30 RESOURCE_LOCK videoout_app0 ENVIRONMENT "APS5_NO_WGI=1;SDL_VIDEODRIVER=dummy")
    if(WIN32)
        set_tests_properties(video_out_flip_unavailable PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(BUILD_TESTING AND NOT WIN32)
    add_test_executable(video_out_frame_dump_queue_tests tests/FrameDumpQueue.cpp)
    target_include_directories(video_out_frame_dump_queue_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_SDL2_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
    target_link_libraries(video_out_frame_dump_queue_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc)
    set_target_properties(video_out_frame_dump_queue_tests PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME video_out_frame_dump_queue COMMAND video_out_frame_dump_queue_tests)
    set_tests_properties(video_out_frame_dump_queue PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0
        ENVIRONMENT "SDL_VIDEODRIVER=${ANYPS5_VIDEO_OUT_TEST_DRIVER};APS5_DUMP_FRAMES=4;APS5_DUMP_SCALE=4")
    add_test(NAME video_out_frame_dump_wait COMMAND video_out_frame_dump_queue_tests wait)
    set_tests_properties(video_out_frame_dump_wait PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0
        ENVIRONMENT "SDL_VIDEODRIVER=${ANYPS5_VIDEO_OUT_TEST_DRIVER};APS5_DUMP_FRAMES=4;APS5_DUMP_SCALE=1")
    add_test(NAME video_out_frame_dump_cpu_queue COMMAND video_out_frame_dump_queue_tests)
    set_tests_properties(video_out_frame_dump_cpu_queue PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0
        ENVIRONMENT "SDL_VIDEODRIVER=${ANYPS5_VIDEO_OUT_TEST_DRIVER};APS5_DUMP_FRAMES=4;APS5_DUMP_SCALE=4;APS5_NO_GPU_DUMP=1")
endif()

if(BUILD_TESTING)
    add_test_executable(pad_sdl_tests tests/PadSdl.cpp)
    target_include_directories(pad_sdl_tests PRIVATE ${LIBS_INCLUDE_DIR} ${videoOutSdlInclude})
    target_link_libraries(pad_sdl_tests PRIVATE libSceVideoOut libScePad libSceIme libSceUserService libc ${videoOutSdl})
    configure_windows_unwind(pad_sdl_tests)
    add_test(NAME pad_sdl COMMAND pad_sdl_tests)
    set_tests_properties(pad_sdl PROPERTIES SKIP_RETURN_CODE 77)
    if(WIN32)
        set_tests_properties(pad_sdl PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceVideoOut>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libScePad>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceIme>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceUserService>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(pad_identify_pad_input_tests tests/PadIdentify.cpp)
    target_include_directories(pad_identify_pad_input_tests PRIVATE ${LIBS_INCLUDE_DIR} ${videoOutSdlInclude})
    target_link_libraries(pad_identify_pad_input_tests PRIVATE libSceVideoOut libScePad libSceIme libSceUserService libc ${videoOutSdl})
    configure_windows_unwind(pad_identify_pad_input_tests)
    foreach(identifyMode connect off rumble invalid)
        add_test(NAME pad_identify_${identifyMode} COMMAND pad_identify_pad_input_tests ${identifyMode})
        set_tests_properties(pad_identify_${identifyMode} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
        if(WIN32)
            set_tests_properties(pad_identify_${identifyMode} PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceVideoOut>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libScePad>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceIme>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceUserService>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        endif()
    endforeach()
    add_test_executable(pad_input_mapping_tests tests/InputMapping.cpp src/InputMapping.cpp)
    target_include_directories(pad_input_mapping_tests PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/3rdparty/SDL2/include)
    target_link_libraries(pad_input_mapping_tests PRIVATE SDL2-static libc)
    set_target_properties(pad_input_mapping_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(pad_input_mapping_tests)
    add_test(NAME pad_input_mapping COMMAND pad_input_mapping_tests)
    if(WIN32)
        set_tests_properties(pad_input_mapping PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(dualsense_status_tests tests/DualSenseStatus.cpp)
    target_include_directories(dualsense_status_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(dualsense_status_tests PRIVATE libc)
    configure_windows_unwind(dualsense_status_tests)
    add_test(NAME dualsense_status COMMAND dualsense_status_tests)
    if(WIN32)
        set_tests_properties(dualsense_status PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(pad_status_sdl_tests tests/PadStatusSdl.cpp src/PadStatusSdl.cpp)
    target_include_directories(pad_status_sdl_tests PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/3rdparty/SDL2/include)
    set_target_properties(pad_status_sdl_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_host_test_executable(pad_status_sdl_tests)
    add_test(NAME pad_status_sdl COMMAND pad_status_sdl_tests)
    add_test_executable(dualsense_haptics_tests tests/DualSenseHaptics.cpp)
    target_include_directories(dualsense_haptics_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(dualsense_haptics_tests PRIVATE libc)
    configure_windows_unwind(dualsense_haptics_tests)
    add_test(NAME dualsense_haptics COMMAND dualsense_haptics_tests)
    if(WIN32)
        set_tests_properties(dualsense_haptics PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
    add_test_executable(pad_output_pacing_tests tests/PadOutputPacing.cpp)
    target_include_directories(pad_output_pacing_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(pad_output_pacing_tests PRIVATE libc)
    configure_windows_unwind(pad_output_pacing_tests)
    add_test(NAME pad_output_pacing COMMAND pad_output_pacing_tests)
    if(WIN32)
        set_tests_properties(pad_output_pacing PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(BUILD_TESTING)
    add_test_executable(pad_host_volume_tests tests/HostVolumeInput.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../libScePad/src/HostVolumeState.cpp)
    target_include_directories(pad_host_volume_tests PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/3rdparty/SDL2/include)
    target_link_libraries(pad_host_volume_tests PRIVATE SDL2-static)
    if(MINGW)
        target_compile_options(pad_host_volume_tests PRIVATE -fasynchronous-unwind-tables)
        target_link_options(pad_host_volume_tests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME pad_host_volume COMMAND pad_host_volume_tests)
endif()

if(BUILD_TESTING)
    add_test_executable(pad_keyboard_mouse_tests tests/KeyboardMouse.cpp src/KeyboardMouse.cpp)
    target_include_directories(pad_keyboard_mouse_tests PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/3rdparty/SDL2/include)
    target_link_libraries(pad_keyboard_mouse_tests PRIVATE SDL2-static)
    if(MINGW)
        target_compile_options(pad_keyboard_mouse_tests PRIVATE -fasynchronous-unwind-tables)
        target_link_options(pad_keyboard_mouse_tests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME pad_keyboard_mouse COMMAND pad_keyboard_mouse_tests)
endif()

if(BUILD_TESTING)
    add_test_executable(guest_video_out_wqhd_detection_tests ${LIBS_INCLUDE_DIR}/tests/GuestVideoOutWqhdDetection.cpp)
    target_include_directories(guest_video_out_wqhd_detection_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_video_out_wqhd_detection_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc)
    configure_windows_unwind(guest_video_out_wqhd_detection_tests)
    add_test(NAME guest_video_out_wqhd_detection COMMAND guest_video_out_wqhd_detection_tests)
    set_tests_properties(guest_video_out_wqhd_detection PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    if(WIN32)
        set_tests_properties(guest_video_out_wqhd_detection PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        add_executable(guest_video_out_wqhd_detection_exit_tests ${LIBS_INCLUDE_DIR}/tests/GuestVideoOutWqhdDetection.cpp)
        set_target_properties(guest_video_out_wqhd_detection_exit_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${ANYPS5_TEST_OUTPUT_DIRECTORY}>")
        target_include_directories(guest_video_out_wqhd_detection_exit_tests PRIVATE ${LIBS_INCLUDE_DIR})
        target_link_libraries(guest_video_out_wqhd_detection_exit_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc)
        target_link_options(guest_video_out_wqhd_detection_exit_tests PRIVATE LINKER:--disable-high-entropy-va)
        configure_windows_unwind(guest_video_out_wqhd_detection_exit_tests)
        add_test(NAME guest_video_out_wqhd_detection_no_vulkan COMMAND guest_video_out_wqhd_detection_exit_tests)
        set_tests_properties(guest_video_out_wqhd_detection_no_vulkan PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0
            ENVIRONMENT "VK_DRIVER_FILES=${CMAKE_CURRENT_BINARY_DIR}/missing_icd.json;VK_ICD_FILENAMES=${CMAKE_CURRENT_BINARY_DIR}/missing_icd.json"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(BUILD_TESTING)
    add_test_executable(guest_video_out_resolution_status_tests ${LIBS_INCLUDE_DIR}/tests/GuestVideoOutResolutionStatus.cpp)
    target_include_directories(guest_video_out_resolution_status_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_video_out_resolution_status_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc)
    configure_windows_unwind(guest_video_out_resolution_status_tests)
    add_test(NAME guest_video_out_resolution_status COMMAND guest_video_out_resolution_status_tests)
    set_tests_properties(guest_video_out_resolution_status PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    if(WIN32)
        set_tests_properties(guest_video_out_resolution_status PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    add_test_executable(guest_video_out_vblank_status_tests ${LIBS_INCLUDE_DIR}/tests/GuestVideoOutVblankStatus.cpp)
    target_include_directories(guest_video_out_vblank_status_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_video_out_vblank_status_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc)
    configure_windows_unwind(guest_video_out_vblank_status_tests)
    add_test(NAME guest_video_out_vblank_status COMMAND guest_video_out_vblank_status_tests)
    set_tests_properties(guest_video_out_vblank_status PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    if(WIN32)
        set_tests_properties(guest_video_out_vblank_status PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(BUILD_TESTING)
    add_test_executable(guest_video_out_output_mode_tests ${LIBS_INCLUDE_DIR}/tests/GuestVideoOutOutputMode.cpp)
    target_include_directories(guest_video_out_output_mode_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_video_out_output_mode_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc)
    configure_windows_unwind(guest_video_out_output_mode_tests)
    add_test(NAME guest_video_out_output_mode COMMAND guest_video_out_output_mode_tests)
    set_tests_properties(guest_video_out_output_mode PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    if(WIN32)
        set_tests_properties(guest_video_out_output_mode PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    add_test_executable(guest_video_out_flip_rate_tests ${LIBS_INCLUDE_DIR}/tests/GuestVideoOutFlipRateBounds.cpp)
    target_include_directories(guest_video_out_flip_rate_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_video_out_flip_rate_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc)
    configure_windows_unwind(guest_video_out_flip_rate_tests)
    add_test(NAME guest_video_out_flip_rate COMMAND guest_video_out_flip_rate_tests)
    set_tests_properties(guest_video_out_flip_rate PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    if(WIN32)
        set_tests_properties(guest_video_out_flip_rate PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    add_test_executable(guest_video_out_host_resolution_limit_tests ${LIBS_INCLUDE_DIR}/tests/GuestVideoOutHostResolutionLimit.cpp)
    target_include_directories(guest_video_out_host_resolution_limit_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_SDL2_INCLUDE_DIR})
    target_link_libraries(guest_video_out_host_resolution_limit_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc)
    configure_windows_unwind(guest_video_out_host_resolution_limit_tests)
    add_test(NAME guest_video_out_host_resolution_limit COMMAND guest_video_out_host_resolution_limit_tests)
    set_tests_properties(guest_video_out_host_resolution_limit PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    add_test(NAME guest_video_out_host_resolution_height_only COMMAND guest_video_out_host_resolution_limit_tests height-only)
    set_tests_properties(guest_video_out_host_resolution_height_only PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    add_test(NAME guest_video_out_host_resolution_width_only COMMAND guest_video_out_host_resolution_limit_tests width-only)
    set_tests_properties(guest_video_out_host_resolution_width_only PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    add_test(NAME guest_video_out_host_resolution_four_k COMMAND guest_video_out_host_resolution_limit_tests four-k)
    set_tests_properties(guest_video_out_host_resolution_four_k PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    add_test(NAME guest_video_out_host_resolution_malformed COMMAND guest_video_out_host_resolution_limit_tests malformed)
    set_tests_properties(guest_video_out_host_resolution_malformed PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    add_test(NAME guest_video_out_host_resolution_disabled COMMAND guest_video_out_host_resolution_limit_tests disabled)
    set_tests_properties(guest_video_out_host_resolution_disabled PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    if(WIN32)
        set_tests_properties(guest_video_out_host_resolution_limit PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        set_tests_properties(guest_video_out_host_resolution_height_only PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        set_tests_properties(guest_video_out_host_resolution_width_only PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        set_tests_properties(guest_video_out_host_resolution_four_k PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        set_tests_properties(guest_video_out_host_resolution_malformed PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
        set_tests_properties(guest_video_out_host_resolution_disabled PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    add_test_executable(guest_video_out_color_space_tests ${LIBS_INCLUDE_DIR}/tests/GuestVideoOutColorSpace.cpp)
    target_include_directories(guest_video_out_color_space_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_video_out_color_space_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc)
    configure_windows_unwind(guest_video_out_color_space_tests)
    add_test(NAME guest_video_out_color_space COMMAND guest_video_out_color_space_tests)
    set_tests_properties(guest_video_out_color_space PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    if(WIN32)
        set_tests_properties(guest_video_out_color_space PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(BUILD_TESTING)
    add_test_executable(guest_video_out_event_id_tests ${LIBS_INCLUDE_DIR}/tests/GuestVideoOutEventId.cpp)
    target_include_directories(guest_video_out_event_id_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_video_out_event_id_tests PRIVATE libSceVideoOut libkernel libc)
    configure_windows_unwind(guest_video_out_event_id_tests)
    add_test(NAME guest_video_out_event_id COMMAND guest_video_out_event_id_tests)
    set_tests_properties(guest_video_out_event_id PROPERTIES TIMEOUT 20)
    if(WIN32)
        set_tests_properties(guest_video_out_event_id PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(BUILD_TESTING)
    add_test_executable(guest_video_out_hdr_tests ${LIBS_INCLUDE_DIR}/tests/GuestVideoOutHdr.cpp)
    target_include_directories(guest_video_out_hdr_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_video_out_hdr_tests PRIVATE libSceVideoOut libSceSystemService libSceAgcDriver libkernel libc)
    configure_windows_unwind(guest_video_out_hdr_tests)
    add_test(NAME guest_video_out_hdr COMMAND guest_video_out_hdr_tests)
    set_tests_properties(guest_video_out_hdr PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    if(WIN32)
        set_tests_properties(guest_video_out_hdr PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    add_test_executable(guest_video_out_set_mode_event_tests ${LIBS_INCLUDE_DIR}/tests/GuestVideoOutSetModeEvent.cpp)
    target_include_directories(guest_video_out_set_mode_event_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_video_out_set_mode_event_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc)
    configure_windows_unwind(guest_video_out_set_mode_event_tests)
    add_test(NAME guest_video_out_set_mode_event COMMAND guest_video_out_set_mode_event_tests)
    set_tests_properties(guest_video_out_set_mode_event PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    if(WIN32)
        set_tests_properties(guest_video_out_set_mode_event PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    add_test_executable(video_out_hdr_probe_tests tests/HdrProbe.cpp)
    target_include_directories(video_out_hdr_probe_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_SDL2_INCLUDE_DIR})
    target_link_libraries(video_out_hdr_probe_tests PRIVATE libSceVideoOut libSceAgcDriver libc ${videoOutSdl})
    if(WIN32)
        target_link_libraries(video_out_hdr_probe_tests PRIVATE dxgi)
    endif()
    set_target_properties(video_out_hdr_probe_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(video_out_hdr_probe_tests)
    add_test(NAME video_out_hdr_probe COMMAND video_out_hdr_probe_tests)
    set_tests_properties(video_out_hdr_probe PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 ENVIRONMENT "SDL_VIDEODRIVER=${ANYPS5_VIDEO_OUT_TEST_DRIVER}")
    if(WIN32)
        set_tests_properties(video_out_hdr_probe PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()

    add_test_executable(video_out_hdr_present_tests tests/HdrPresent.cpp)
    target_include_directories(video_out_hdr_present_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_SDL2_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
    target_link_libraries(video_out_hdr_present_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc)
    set_target_properties(video_out_hdr_present_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(video_out_hdr_present_tests)
    add_test(NAME video_out_hdr_present COMMAND video_out_hdr_present_tests)
    set_tests_properties(video_out_hdr_present PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0)
    if(WIN32)
        set_tests_properties(video_out_hdr_present PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(BUILD_TESTING)
    add_test_executable(guest_buffer_reuse_tests ${LIBS_INCLUDE_DIR}/tests/GuestBufferReuse.cpp)
    target_include_directories(guest_buffer_reuse_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_buffer_reuse_tests PRIVATE libc)
    configure_windows_unwind(guest_buffer_reuse_tests)
    add_test(NAME guest_buffer_reuse COMMAND guest_buffer_reuse_tests)
    if(WIN32)
        set_tests_properties(guest_buffer_reuse PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(BUILD_TESTING)
    add_test_executable(video_out_telemetry_tests tests/Telemetry.cpp)
    target_include_directories(video_out_telemetry_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_SDL2_INCLUDE_DIR})
    target_link_libraries(video_out_telemetry_tests PRIVATE libSceVideoOut libSceAgcDriver libkernel libc ${videoOutSdl})
    set_target_properties(video_out_telemetry_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(video_out_telemetry_tests)
    add_test(NAME video_out_telemetry COMMAND video_out_telemetry_tests)
    add_test(NAME video_out_telemetry_caption COMMAND video_out_telemetry_tests caption)
    set_tests_properties(video_out_telemetry PROPERTIES TIMEOUT 30)
    set_tests_properties(video_out_telemetry_caption PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 RESOURCE_LOCK videoout_app0 ENVIRONMENT "SDL_VIDEODRIVER=${ANYPS5_VIDEO_OUT_TEST_DRIVER}")
    if(WIN32)
        set_tests_properties(video_out_telemetry video_out_telemetry_caption PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

if(BUILD_TESTING)
    add_test_executable(video_out_port0_presence_tests tests/Port0Presence.cpp)
    target_include_directories(video_out_port0_presence_tests PRIVATE ${LIBS_INCLUDE_DIR})
    configure_host_test_executable(video_out_port0_presence_tests)
    add_test(NAME video_out_port0_presence COMMAND video_out_port0_presence_tests)
    set_tests_properties(video_out_port0_presence PROPERTIES TIMEOUT 30)
endif()

if(BUILD_TESTING)
    add_test_executable(pad_port0_tests tests/PadPort0.cpp)
    target_include_directories(pad_port0_tests PRIVATE ${LIBS_INCLUDE_DIR} ${videoOutSdlInclude})
    target_link_libraries(pad_port0_tests PRIVATE libSceVideoOut libScePad libSceIme libSceUserService libc ${videoOutSdl})
    configure_windows_unwind(pad_port0_tests)
    add_test(NAME pad_port0 COMMAND pad_port0_tests)
    set_tests_properties(pad_port0 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    if(WIN32)
        set_tests_properties(pad_port0 PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceVideoOut>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libScePad>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceIme>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceUserService>;PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

# nps5: static flavours for the native arm64 macOS link (SDL2 + Vulkan window, no guest .prx packaging).
# EXCLUDE_FROM_ALL. nps5_VideoOut is the three requested libraries' VideoOut part;
# nps5_VideoOutDeps holds the Mouse/Keyboard/Ime sources VideoOut calls into
# (sources live in their own dirs; compile unchanged on arm64).
set(NPS5_VIDEO_SRC ${CMAKE_CURRENT_SOURCE_DIR})
add_library(nps5_VideoOut STATIC EXCLUDE_FROM_ALL
    ${NPS5_VIDEO_SRC}/Unimplemented.cpp
    ${NPS5_VIDEO_SRC}/InventoryExports.cpp
    ${NPS5_VIDEO_SRC}/src/Event.cpp
    ${NPS5_VIDEO_SRC}/src/Buffer.cpp
    ${NPS5_VIDEO_SRC}/src/Output.cpp
    ${NPS5_VIDEO_SRC}/src/VideoOutDriver.cpp
    ${NPS5_VIDEO_SRC}/src/DisplayWindow.cpp
    ${NPS5_VIDEO_SRC}/src/HostFullscreen.cpp
    ${NPS5_VIDEO_SRC}/src/PadInput.cpp
    ${NPS5_VIDEO_SRC}/src/PadStatusSdl.cpp
    ${NPS5_VIDEO_SRC}/src/Telemetry.cpp
    ${NPS5_VIDEO_SRC}/src/MouseInput.cpp
    ${NPS5_VIDEO_SRC}/src/KeyboardInput.cpp
    ${NPS5_VIDEO_SRC}/src/ImeTextInput.cpp
    ${NPS5_VIDEO_SRC}/src/DialogInput.cpp
    ${NPS5_VIDEO_SRC}/src/InputMapping.cpp
    ${NPS5_VIDEO_SRC}/src/KeyboardMouse.cpp
    ${NPS5_VIDEO_SRC}/src/HostHdrProbe.cpp
    ${NPS5_VIDEO_SRC}/src/DisplayLinkBeat.cpp
    ${NPS5_VIDEO_SRC}/../libkernel/Time/TimedWait.cpp
)
target_include_directories(nps5_VideoOut PRIVATE
    ${CMAKE_SOURCE_DIR}/core/Decoder/Png/include ${LIBS_INCLUDE_DIR} ${ANYPS5_SDL2_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR}
    ${CMAKE_BINARY_DIR}/core/libs/prx/libSceAgcDriver/generated)
add_dependencies(nps5_VideoOut agc_trace_revision)
set_target_properties(nps5_VideoOut PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)

add_library(nps5_VideoOutDeps STATIC EXCLUDE_FROM_ALL
    ${NPS5_VIDEO_SRC}/../libSceMouse/InventoryExports.cpp
    ${NPS5_VIDEO_SRC}/../libSceMouse/Export.cpp
    ${NPS5_VIDEO_SRC}/../libSceMouse/src/mouse_impl.cpp
    ${NPS5_VIDEO_SRC}/../libSceKeyboard/Export.cpp
    ${NPS5_VIDEO_SRC}/../libSceKeyboard/PadEmulate.cpp
    ${NPS5_VIDEO_SRC}/../libSceKeyboard/src/keyboard_impl.cpp
    ${NPS5_VIDEO_SRC}/../libSceIme/Export.cpp
    ${NPS5_VIDEO_SRC}/../libSceIme/src/ModalDialog.cpp
    ${NPS5_VIDEO_SRC}/../libSceIme/src/PsnDialogNotice.cpp
)
target_include_directories(nps5_VideoOutDeps PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_SDL2_INCLUDE_DIR})
set_target_properties(nps5_VideoOutDeps PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)

# Host smoke test: real DisplayWindow + Vulkan clear/present + pad/user-service poll, no guest code.
add_executable(nps5_video_pad_smoke EXCLUDE_FROM_ALL ${NPS5_VIDEO_SRC}/smoke/VideoPadSmoke.cpp ${NPS5_VIDEO_SRC}/smoke/HostStubs.cpp)
target_include_directories(nps5_video_pad_smoke PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_SDL2_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/core/Decoder/Png/include)
target_link_libraries(nps5_video_pad_smoke PRIVATE nps5_VideoOut nps5_VideoOutDeps nps5_Pad nps5_UserService libSceAgcDriver libkernel libc decoder_png SDL2-static "-framework Cocoa" "-framework CoreFoundation" "-framework CoreVideo" "-framework CoreGraphics")
set_target_properties(nps5_video_pad_smoke PROPERTIES CXX_EXTENSIONS OFF)

# nps5: one dylib holding VideoOut, Pad, UserService (+ Mouse/Keyboard/Ime) with their exported names.
# The nativeps5 runtime defines C functions with the same names (its own HLE); a dylib keeps the two sets apart
# (the runtime reaches these through dlsym on this image, never through the linker). Every object is loaded
# (-force_load) so every export is present. docs/nps5-int.md.
# KytyHost.cpp: helpers the runtime's NPS5_GPU=kyty seam reaches through dlsym (event queues, pad, exit); unused otherwise.
add_library(nps5_gpu SHARED EXCLUDE_FROM_ALL ${CMAKE_SOURCE_DIR}/core/libs/nps5/GpuBridge.cpp ${CMAKE_SOURCE_DIR}/core/libs/nps5/KytyHost.cpp)
target_include_directories(nps5_gpu PRIVATE ${LIBS_INCLUDE_DIR})
add_dependencies(nps5_gpu nps5_VideoOut nps5_VideoOutDeps nps5_Pad nps5_UserService)
target_link_libraries(nps5_gpu PRIVATE
    "-Wl,-force_load,$<TARGET_FILE:nps5_VideoOut>" "-Wl,-force_load,$<TARGET_FILE:nps5_VideoOutDeps>"
    "-Wl,-force_load,$<TARGET_FILE:nps5_Pad>" "-Wl,-force_load,$<TARGET_FILE:nps5_UserService>"
    libSceAgcDriver libkernel libc decoder_png SDL2-static "-framework Cocoa" "-framework CoreFoundation" "-framework CoreVideo" "-framework CoreGraphics")
set_target_properties(nps5_gpu PROPERTIES CXX_EXTENSIONS OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/core/libs/libs/unpatched BUILD_RPATH "@loader_path")

# nps5: SDL controller host test (virtual joysticks stand in for DualSense / Switch Pro / Xbox). Needs no hardware or window.
add_executable(nps5_pad_host_tests EXCLUDE_FROM_ALL ${NPS5_VIDEO_SRC}/tests/Nps5PadHost.cpp ${NPS5_VIDEO_SRC}/smoke/HostStubs.cpp)
target_include_directories(nps5_pad_host_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_SDL2_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/core/Decoder/Png/include)
target_link_libraries(nps5_pad_host_tests PRIVATE nps5_VideoOut nps5_VideoOutDeps nps5_Pad nps5_UserService libSceAgcDriver libkernel libc decoder_png SDL2-static "-framework Cocoa" "-framework CoreFoundation" "-framework CoreVideo" "-framework CoreGraphics")
set_target_properties(nps5_pad_host_tests PROPERTIES CXX_EXTENSIONS OFF)
