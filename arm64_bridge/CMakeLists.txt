cmake_minimum_required(VERSION 3.22)
project(arm32_bridge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Android's bionic ELF TLS descriptors are not usable on the API-25 device
# targeted by this build. Keep compiler-emulated TLS here; the bridge uses
# thread_local in its bootstrap path, so a native TLS descriptor would leave a
# null resolver and crash before the guest can start audio.
add_compile_options(-fvisibility=hidden -fvisibility-inlines-hidden
                    -ffunction-sections -fdata-sections)
add_link_options(-Wl,--gc-sections -Wl,--icf=all -Wl,--exclude-libs,ALL)

set(BOOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost")
set(BOOST_INCLUDEDIR "${BOOST_ROOT}")
set(Boost_INCLUDE_DIR "${BOOST_ROOT}")
set(Boost_NO_SYSTEM_PATHS ON)
set(Boost_FOUND TRUE)
include_directories("${BOOST_ROOT}")

set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)

message(STATUS "Configuring pinned Dynarmic dependency...")
FetchContent_Declare(
    dynarmic
    GIT_REPOSITORY https://github.com/azahar-emu/dynarmic.git
    GIT_TAG        e77b1ba0b7da7cbe93021b01a663acfe7c4dd516
    GIT_CONFIG    core.autocrlf=false
    PATCH_COMMAND git apply --whitespace=nowarn
                  "${CMAKE_CURRENT_SOURCE_DIR}/patches/dynarmic-arm64.patch"
)
FetchContent_MakeAvailable(dynarmic)

set(BRIDGE_TARGET arm32_bridge CACHE STRING "Bridge library target name")
set(BRIDGE_SOURCES
    third_party/texture2ddecoder/astc.cpp
    third_party/texture2ddecoder/etc.cpp
    src/elf_loader.cpp
    src/dynarmic_host.cpp
    src/thunk_manager.cpp
    src/etc1_encoder.cpp
    src/thunks_gl.cpp
    src/thunks_gl_gles2_core.cpp
    src/thunks_libc.cpp
    src/thunks_libc_extra.cpp
    src/thunks_network.cpp
    src/thunks_zlib.cpp
    src/fmod_host_decode.cpp
    src/guest_signals.cpp
    src/guest_cp15.cpp
    src/thunks_android.cpp
    src/thunks_egl.cpp
    src/fxaa_present.cpp
    src/apk_reader.cpp
    src/game_data_vfs.cpp
    src/mod_astc_encoder.cpp
    src/mod_texture_bridge.cpp
    src/mod_texture_conversion.cpp
    src/warband_extra_thunks.cpp
    src/bridge_config.cpp
    src/guest_jnienv.cpp
    src/unity_jni.cpp
    src/native_activity_bridge.cpp
    src/thunks_opensles.cpp
    src/russian_text_processor.cpp
    src/jni_bridge.cpp
)

add_library(${BRIDGE_TARGET} SHARED ${BRIDGE_SOURCES})

# Desktop Warband modules commonly contain DXT DDS textures. On devices that
# expose ASTC, encode those decoded mip levels back to native ASTC in-process;
# the existing ETC1/RGBA compatibility path remains the fallback for devices
# without ASTC. Only 4x4 and 6x6 footprints are needed by the guest loader.
set(ASTCENC_CLI OFF CACHE BOOL "" FORCE)
set(ASTCENC_DECOMPRESSOR OFF CACHE BOOL "" FORCE)
set(ASTCENC_SHAREDLIB OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNITTEST OFF CACHE BOOL "" FORCE)
set(ASTCENC_DIAGNOSTICS OFF CACHE BOOL "" FORCE)
set(ASTCENC_WERROR OFF CACHE BOOL "" FORCE)
set(ASTCENC_BLOCK_MAX_TEXELS 36 CACHE STRING "" FORCE)
if(ANDROID OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(ASTCENC_ISA_NEON ON CACHE BOOL "" FORCE)
    set(ASTCENC_ISA_NONE OFF CACHE BOOL "" FORCE)
    set(WARBAND_ASTCENC_TARGET astcenc-neon-static)
else()
    set(ASTCENC_ISA_NEON OFF CACHE BOOL "" FORCE)
    set(ASTCENC_ISA_NONE ON CACHE BOOL "" FORCE)
    set(WARBAND_ASTCENC_TARGET astcenc-none-static)
endif()
set(ASTCENC_ISA_NATIVE OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_AVX2 OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_SSE41 OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_SSE2 OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_SVE_128 OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_SVE_256 OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/astcenc EXCLUDE_FROM_ALL)

option(WARBAND_BRIDGE_ENABLE_LOGS "Emit Android logs from the release bridge" ON)
if(NOT WARBAND_BRIDGE_ENABLE_LOGS)
    target_compile_options(${BRIDGE_TARGET} PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge_no_logs.h")
    # Dynarmic's Oaknut headers contain Android logging calls in inline
    # range-check paths. Apply the same release policy to the dependency so
    # -Wl,--no-undefined remains valid without linking liblog.
    target_compile_options(dynarmic PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/src/bridge_no_logs.h")
endif()

target_include_directories(${BRIDGE_TARGET} PRIVATE
    src
    third_party/texture2ddecoder
    "${dynarmic_SOURCE_DIR}/include"
    "${BOOST_ROOT}"
)

target_link_libraries(${BRIDGE_TARGET} PRIVATE
    dynarmic
    ${WARBAND_ASTCENC_TARGET}
    GLESv2
    GLESv3
    EGL
    OpenSLES
    mediandk
    android
    dl
    m
    z
)

if(WARBAND_BRIDGE_ENABLE_LOGS)
    target_link_libraries(${BRIDGE_TARGET} PRIVATE log)
endif()

target_compile_definitions(${BRIDGE_TARGET} PRIVATE ANDROID __ANDROID__)

target_link_options(${BRIDGE_TARGET} PRIVATE
    "-Wl,-z,max-page-size=16384"
    "-Wl,-z,common-page-size=16384"
    "-Wl,-z,relro"
    "-Wl,-z,now"
)

# These header-only input paths sit directly on Android's event thread. Compile their
# executable specifications in every bridge build so API drift cannot reach an APK.
option(WARBAND_BUILD_INPUT_TESTS "Compile pure input contract tests" ON)
if(WARBAND_BUILD_INPUT_TESTS)
    add_subdirectory(tests)
endif()
