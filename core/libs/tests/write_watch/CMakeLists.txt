cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(WriteWatchTests LANGUAGES CXX)
    enable_testing()
    set(ANYPS5_TEST_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()

find_package(Threads REQUIRED)
add_executable(write_watch_backing_tests ../WriteWatchBacking.cpp ../../prx/libc/src/GuestWriteWatch.cpp)
target_include_directories(write_watch_backing_tests PRIVATE ../..)
target_compile_features(write_watch_backing_tests PRIVATE cxx_std_20)
target_link_libraries(write_watch_backing_tests PRIVATE Threads::Threads)
set_target_properties(write_watch_backing_tests PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "$<1:${ANYPS5_TEST_OUTPUT_DIRECTORY}>")

foreach(mode shared private writable late restore)
    add_test(NAME write_watch_backing_${mode} COMMAND write_watch_backing_tests ${mode})
    set_tests_properties(write_watch_backing_${mode} PROPERTIES TIMEOUT 30)
endforeach()

add_executable(write_watch_tests ../WriteWatch.cpp ../../prx/libc/src/GuestWriteWatch.cpp)
target_include_directories(write_watch_tests PRIVATE ../..)
target_compile_features(write_watch_tests PRIVATE cxx_std_20)
target_link_libraries(write_watch_tests PRIVATE Threads::Threads)
target_compile_definitions(write_watch_tests PRIVATE APS5_WRITE_WATCH_TEST_HOOKS=1)
set_target_properties(write_watch_tests PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "$<1:${ANYPS5_TEST_OUTPUT_DIRECTORY}>")

foreach(mode collect fresh armed register mapped host desync protection-trapped access-trap-race logical empty protection overlaps threads raised fault-ahead read-trap read-trap-threads read-trap-holder read-trap-bypass read-trap-import read-trap-fault read-trap-late read-trap-fault-bound host-runtime unmap-order host-write-store host-write-token protect-keeping-traps layout-resync)
    add_test(NAME write_watch_${mode} COMMAND write_watch_tests ${mode})
    set_tests_properties(write_watch_${mode} PROPERTIES TIMEOUT 30)
endforeach()

add_test(NAME write_watch_fault-ahead-off COMMAND write_watch_tests fault-ahead-off)
set_tests_properties(write_watch_fault-ahead-off PROPERTIES TIMEOUT 30 ENVIRONMENT APS5_WRITE_WATCH_FAULT_AHEAD=0)

foreach(mode execute-fault read-fault)
    add_test(NAME write_watch_${mode} COMMAND write_watch_tests ${mode})
    set_tests_properties(write_watch_${mode} PROPERTIES TIMEOUT 30)
endforeach()

add_executable(write_watch_stress_tests ../WriteWatchStress.cpp ../../prx/libc/src/GuestWriteWatch.cpp)
target_include_directories(write_watch_stress_tests PRIVATE ../..)
target_compile_features(write_watch_stress_tests PRIVATE cxx_std_20)
target_link_libraries(write_watch_stress_tests PRIVATE Threads::Threads)
set_target_properties(write_watch_stress_tests PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "$<1:${ANYPS5_TEST_OUTPUT_DIRECTORY}>")

foreach(mode query edge fuzz race-collect race-query)
    add_test(NAME write_watch_stress_${mode} COMMAND write_watch_stress_tests ${mode})
    set_tests_properties(write_watch_stress_${mode} PROPERTIES TIMEOUT 60)
endforeach()
add_test(NAME write_watch_stress_fuzz-exact COMMAND write_watch_stress_tests fuzz-exact)
set_tests_properties(write_watch_stress_fuzz-exact PROPERTIES TIMEOUT 60 ENVIRONMENT APS5_WRITE_WATCH_FAULT_AHEAD=0)
add_test(NAME write_watch_stress_bench COMMAND write_watch_stress_tests bench)
set_tests_properties(write_watch_stress_bench PROPERTIES TIMEOUT 120)
