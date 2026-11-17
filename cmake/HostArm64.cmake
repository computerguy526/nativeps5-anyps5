# nps5: dependency wiring for the native arm64 macOS configuration (APS5_HOST_ARM64=ON).
#
# The x86_64 configuration builds SDL2, zstd, zlib, glslang, ... from the 3rdparty submodules.
# The arm64 configuration links the Homebrew builds instead (brew install sdl2 zstd glslang)
# and creates the same target names the library CMakeLists use, so they need no changes.
# See docs/nps5-arm64.md.

enable_language(C)

set(ANYPS5_BREW_PREFIX "/opt/homebrew" CACHE PATH "Homebrew prefix that provides the arm64 host libraries")
list(APPEND CMAKE_PREFIX_PATH "${ANYPS5_BREW_PREFIX}")

# SDL2 (sdl2 or sdl2-compat; only the SDL2 API is used)
find_package(SDL2 CONFIG REQUIRED)
if(NOT TARGET SDL2::SDL2)
    message(FATAL_ERROR "APS5_HOST_ARM64: SDL2 was found but does not provide SDL2::SDL2")
endif()
add_library(SDL2-static INTERFACE)
target_link_libraries(SDL2-static INTERFACE SDL2::SDL2)
get_target_property(ANYPS5_SDL2_INCLUDE_DIR SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)

# zstd: the headers come from the submodule (include paths already point there), the library from brew.
find_library(ANYPS5_ZSTD_LIBRARY NAMES libzstd.a zstd REQUIRED)
add_library(libzstd_static INTERFACE)
target_link_libraries(libzstd_static INTERFACE "${ANYPS5_ZSTD_LIBRARY}")
target_include_directories(libzstd_static INTERFACE "${CMAKE_SOURCE_DIR}/3rdparty/zstd/lib")

# zlib: the macOS SDK libz (host zlib; the guest zlib lives in libSceNKWebKitRequirements and is not built here)
find_package(ZLIB REQUIRED)

# CoreFoundation, used by libkernel's main-thread dispatch
find_library(ANYPS5_COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)

# ZArchive (app0 archive reader in libc) on top of the brew zstd
set(ZARCHIVE_SOURCE_DIR ${CMAKE_SOURCE_DIR}/3rdparty/ZArchive)
add_library(zarchive STATIC EXCLUDE_FROM_ALL
    ${ZARCHIVE_SOURCE_DIR}/src/zarchivereader.cpp
    ${ZARCHIVE_SOURCE_DIR}/src/zarchivewriter.cpp
    ${ZARCHIVE_SOURCE_DIR}/src/sha_256.c
)
target_include_directories(zarchive SYSTEM PUBLIC ${ZARCHIVE_SOURCE_DIR}/include)
target_compile_features(zarchive PUBLIC cxx_std_20)
target_link_libraries(zarchive PRIVATE libzstd_static)
set_target_properties(zarchive PROPERTIES POSITION_INDEPENDENT_CODE ON)

# glslang (the recompiler links the library; the agc driver also wants the standalone compiler target)
find_package(glslang CONFIG REQUIRED)
add_library(glslang INTERFACE)
target_link_libraries(glslang INTERFACE glslang::glslang)
add_library(SPIRV INTERFACE)
target_link_libraries(SPIRV INTERFACE glslang::SPIRV)
find_path(ANYPS5_GLSLANG_PUBLIC_INCLUDE_DIR NAMES glslang/Public/ShaderLang.h REQUIRED)
set(ANYPS5_GLSLANG_SPIRV_INCLUDE_DIR "${ANYPS5_GLSLANG_PUBLIC_INCLUDE_DIR}/glslang")
set(ANYPS5_GLSLANG_INCLUDE_DIRS "${ANYPS5_GLSLANG_PUBLIC_INCLUDE_DIR};${ANYPS5_GLSLANG_SPIRV_INCLUDE_DIR}")
find_program(ANYPS5_GLSLANG_STANDALONE_EXECUTABLE NAMES glslang glslangValidator REQUIRED)
add_executable(glslang-standalone IMPORTED GLOBAL)
set_target_properties(glslang-standalone PROPERTIES IMPORTED_LOCATION "${ANYPS5_GLSLANG_STANDALONE_EXECUTABLE}")

# Vulkan-Headers and SPIRV-Headers stay on the bundled submodule copies (set by cmake/SystemDeps.cmake).

# Decoders used by the agc driver (screenshots) and the Png/Jpeg prx libs.
add_subdirectory(core/Decoder/Jpeg)
add_subdirectory(core/Decoder/Png)
