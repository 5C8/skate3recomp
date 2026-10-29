cmake_minimum_required(VERSION 3.25)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

option(SKATE3_STATIC_MSVC_RUNTIME
    "Link Windows MSVC-ABI builds with the static Visual C++ runtime"
    OFF)
if(SKATE3_STATIC_MSVC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(skate3 VERSION 1.0.2 LANGUAGES CXX)

if(WIN32)
    enable_language(RC)
endif()
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/Skate3Version.cmake)

# Pinned to the project version: the repository's v* tags are the upstream
# Skate3Recomp releases (v2.0.2 ...), not this build's, so git-describe
# versioning (skate3_resolve_version) would stamp their number on it.
set(SKATE3_FULL_VERSION "${PROJECT_VERSION}")
string(REGEX REPLACE "-.*$" "" SKATE3_NUMERIC_VERSION "${SKATE3_FULL_VERSION}")

# Four-part comma-separated form for Windows VERSIONINFO resources.
string(REPLACE "." ";" SKATE3_RC_VERSION_PARTS "${SKATE3_NUMERIC_VERSION}")
list(LENGTH SKATE3_RC_VERSION_PARTS SKATE3_RC_VERSION_PART_COUNT)
while(SKATE3_RC_VERSION_PART_COUNT LESS 4)
    list(APPEND SKATE3_RC_VERSION_PARTS 0)
    list(LENGTH SKATE3_RC_VERSION_PARTS SKATE3_RC_VERSION_PART_COUNT)
endwhile()
list(JOIN SKATE3_RC_VERSION_PARTS "," SKATE3_RC_VERSION)

set(SKATE3_TARGET_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES SKATE3_OSX_ARCH_COUNT)
    if(SKATE3_OSX_ARCH_COUNT EQUAL 1)
        set(SKATE3_TARGET_PROCESSOR "${CMAKE_OSX_ARCHITECTURES}")
    endif()
endif()

if(WIN32)
    if(SKATE3_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
        set(SKATE3_BUILD_PLATFORM "win-amd64")
    elseif(SKATE3_TARGET_PROCESSOR MATCHES "aarch64|ARM64")
        set(SKATE3_BUILD_PLATFORM "win-arm64")
    else()
        set(SKATE3_BUILD_PLATFORM "win-${SKATE3_TARGET_PROCESSOR}")
    endif()
elseif(APPLE)
    if(SKATE3_TARGET_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        set(SKATE3_BUILD_PLATFORM "macos-arm64")
    else()
        message(FATAL_ERROR "Unsupported macOS architecture: ${SKATE3_TARGET_PROCESSOR}. macOS builds are ARM64-only.")
    endif()
elseif(UNIX AND NOT APPLE)
    if(SKATE3_TARGET_PROCESSOR MATCHES "AMD64|x86_64")
        set(SKATE3_BUILD_PLATFORM "linux-amd64")
    elseif(SKATE3_TARGET_PROCESSOR MATCHES "aarch64|ARM64")
        set(SKATE3_BUILD_PLATFORM "linux-arm64")
    else()
        set(SKATE3_BUILD_PLATFORM "linux-${SKATE3_TARGET_PROCESSOR}")
    endif()
else()
    set(SKATE3_BUILD_PLATFORM "${CMAKE_SYSTEM_NAME}-${SKATE3_TARGET_PROCESSOR}")
endif()

string(TIMESTAMP SKATE3_BUILD_TIMESTAMP "%Y%m%d_%H%M")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/skate3_version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/skate3_version.h
    @ONLY
)

set(REXSDK_DIR
    ""
    CACHE PATH "Path to the rexglue SDK source tree")
set(SKATE3_GAME_DATA_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/game"
    CACHE PATH "Path to an extracted Skate 3 retail game dump")
set(SKATE3_TITLE_UPDATE_PACKAGE
    ""
    CACHE FILEPATH "Optional Skate 3 title update STFS package used for codegen")
option(SKATE3_REQUIRE_TITLE_UPDATE
    "Require Title Update 3 game images or matching runtime patches" ON)
option(SKATE3_ENABLE_X86_64_V3
    "Build x86_64 desktop binaries for x86-64-v3 CPUs (AVX2/BMI2)"
    OFF)

if(NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    if(SKATE3_ENABLE_X86_64_V3)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            add_compile_options(-march=x86-64-v3)
            set(SNAPPY_REQUIRE_AVX2 ON CACHE BOOL
                "Match Snappy feature detection to the project x86-64-v3 target" FORCE)
            set(SNAPPY_HAVE_SSSE3 1 CACHE INTERNAL
                "x86-64-v3 requires SSSE3; Snappy checks do not see directory compile options" FORCE)
            set(SNAPPY_HAVE_X86_CRC32 1 CACHE INTERNAL
                "x86-64-v3 requires SSE4.2 CRC32; Snappy checks do not see directory compile options" FORCE)
            set(SNAPPY_HAVE_BMI2 1 CACHE INTERNAL
                "x86-64-v3 requires BMI2; Snappy checks do not see directory compile options" FORCE)
        elseif(MSVC)
            add_compile_options(/arch:AVX2)
        endif()
    else()
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            add_compile_options(-march=x86-64 -mtune=generic -mssse3)
        endif()
        set(SNAPPY_REQUIRE_AVX OFF CACHE BOOL
            "Do not require AVX for non-AVX x86_64 release builds" FORCE)
        set(SNAPPY_REQUIRE_AVX2 OFF CACHE BOOL
            "Do not require AVX2 for non-AVX x86_64 release builds" FORCE)
        set(SNAPPY_HAVE_SSSE3 0 CACHE INTERNAL
            "Keep Snappy on its scalar path for non-AVX x86_64 release builds" FORCE)
        set(SNAPPY_HAVE_X86_CRC32 0 CACHE INTERNAL
            "Non-AVX x86_64 release builds must not require SSE4.2 CRC32" FORCE)
        set(SNAPPY_HAVE_BMI2 0 CACHE INTERNAL
            "Non-AVX x86_64 release builds must not require BMI2" FORCE)
    endif()
else()
    set(SNAPPY_REQUIRE_AVX OFF CACHE BOOL
        "Do not require AVX for generic release builds" FORCE)
    set(SNAPPY_REQUIRE_AVX2 OFF CACHE BOOL
        "Do not require AVX2 for generic release builds" FORCE)
    set(SNAPPY_HAVE_SSSE3 0 CACHE INTERNAL
        "Generic release builds must not require SSSE3" FORCE)
    set(SNAPPY_HAVE_X86_CRC32 0 CACHE INTERNAL
        "Generic release builds must not require SSE4.2 CRC32" FORCE)
    set(SNAPPY_HAVE_BMI2 0 CACHE INTERNAL
        "Generic release builds must not require BMI2" FORCE)
endif()

file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" SKATE3_SOURCE_DIR_CONFIG)
file(TO_CMAKE_PATH "${SKATE3_GAME_DATA_ROOT}" SKATE3_GAME_DATA_ROOT_CONFIG)

set(SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE "${SKATE3_TITLE_UPDATE_PACKAGE}")
if(SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE STREQUAL "")
    set(SKATE3_LOCAL_TITLE_UPDATE_PACKAGE
        "${CMAKE_CURRENT_SOURCE_DIR}/TU_12K2276_000000C000000.00000000000O3")
    if(EXISTS "${SKATE3_LOCAL_TITLE_UPDATE_PACKAGE}")
        set(SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE "${SKATE3_LOCAL_TITLE_UPDATE_PACKAGE}")
    endif()
endif()

set(SKATE3_PATCHED_DEFAULT_XEX_LINE "")
set(SKATE3_PATCHED_EAWEBKIT_XEX_LINE "")
set(SKATE3_CODEGEN_ARGS "")
set(SKATE3_FUNCTION_CONFIG_INCLUDES
    "  \"${SKATE3_SOURCE_DIR_CONFIG}/config/skate3_functions.toml\",")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(NOT SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE STREQUAL "")
    if(NOT SKATE3_REQUIRE_TITLE_UPDATE)
        message(FATAL_ERROR "Title-update codegen requires SKATE3_REQUIRE_TITLE_UPDATE=ON")
    endif()
    if(NOT EXISTS "${SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE}")
        message(FATAL_ERROR
            "SKATE3_TITLE_UPDATE_PACKAGE does not exist: ${SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE}")
    endif()

    set(SKATE3_TITLE_UPDATE_CODEGEN_ROOT
        "${CMAKE_CURRENT_BINARY_DIR}/title_update_codegen")
    set(SKATE3_TITLE_UPDATE_DEFAULT_XEX
        "${SKATE3_TITLE_UPDATE_CODEGEN_ROOT}/default.xex")
    set(SKATE3_TITLE_UPDATE_DEFAULT_XEXP
        "${SKATE3_TITLE_UPDATE_CODEGEN_ROOT}/default.xexp")
    set(SKATE3_TITLE_UPDATE_EAWEBKIT_XEX
        "${SKATE3_TITLE_UPDATE_CODEGEN_ROOT}/data/webkit/EAWebkit.xex")
    set(SKATE3_TITLE_UPDATE_EAWEBKIT_XEXP
        "${SKATE3_TITLE_UPDATE_CODEGEN_ROOT}/data/webkit/EAWebkit.xexp")

    add_custom_command(
        OUTPUT
            "${SKATE3_TITLE_UPDATE_DEFAULT_XEX}"
            "${SKATE3_TITLE_UPDATE_DEFAULT_XEXP}"
            "${SKATE3_TITLE_UPDATE_EAWEBKIT_XEX}"
            "${SKATE3_TITLE_UPDATE_EAWEBKIT_XEXP}"
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExtractTitleUpdateXexp.py"
            --tu "${SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE}"
            --game-root "${SKATE3_GAME_DATA_ROOT}"
            --out-root "${SKATE3_TITLE_UPDATE_CODEGEN_ROOT}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExtractTitleUpdateXexp.py"
            "${SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE}"
            "${SKATE3_GAME_DATA_ROOT}/default.xex"
            "${SKATE3_GAME_DATA_ROOT}/data/webkit/EAWebkit.xex"
        COMMENT "Staging Skate 3 title update patches for codegen"
        VERBATIM
    )
    add_custom_target(skate3-title-update-codegen-inputs
        DEPENDS
            "${SKATE3_TITLE_UPDATE_DEFAULT_XEX}"
            "${SKATE3_TITLE_UPDATE_DEFAULT_XEXP}"
            "${SKATE3_TITLE_UPDATE_EAWEBKIT_XEX}"
            "${SKATE3_TITLE_UPDATE_EAWEBKIT_XEXP}"
    )

    file(TO_CMAKE_PATH "${SKATE3_TITLE_UPDATE_DEFAULT_XEX}"
        SKATE3_TITLE_UPDATE_DEFAULT_XEX_CONFIG)
    file(TO_CMAKE_PATH "${SKATE3_TITLE_UPDATE_EAWEBKIT_XEX}"
        SKATE3_TITLE_UPDATE_EAWEBKIT_XEX_CONFIG)
    set(SKATE3_PATCHED_DEFAULT_XEX_LINE
        "patched_file_path = \"${SKATE3_TITLE_UPDATE_DEFAULT_XEX_CONFIG}\"")
    set(SKATE3_PATCHED_EAWEBKIT_XEX_LINE
        "patched_file_path = \"${SKATE3_TITLE_UPDATE_EAWEBKIT_XEX_CONFIG}\"")
    set(SKATE3_FUNCTION_CONFIG_INCLUDES
        "  \"${SKATE3_SOURCE_DIR_CONFIG}/config/skate3_tu_functions.toml\",")
    message(STATUS
        "Skate 3 title update codegen enabled: ${SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE}")
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/manifests")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/manifests/skate3.toml.in
    ${CMAKE_CURRENT_BINARY_DIR}/manifests/skate3.toml
    @ONLY
)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/manifests/eawebkit.toml.in
    ${CMAKE_CURRENT_BINARY_DIR}/manifests/eawebkit.toml
    @ONLY
)
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "Disable Tracy instrumentation for normal Skate 3 builds" FORCE)
set(REXGLUE_ENABLE_PERF_COUNTERS OFF CACHE BOOL "Enable lightweight rexglue perf counter instrumentation")

if(WIN32)
    set(REXGLUE_RUNTIME_RC_VERSION "${SKATE3_RC_VERSION}")
    set(REXGLUE_RUNTIME_RC_VERSION_STRING "${SKATE3_FULL_VERSION}")
    set(REXGLUE_RUNTIME_RC_PRODUCT_NAME "Skate3Recomp")
    set(REXGLUE_RUNTIME_RC_COMPANY "Skate3Recomp project")
    set(REXGLUE_RUNTIME_RC_DESCRIPTION "Skate3Recomp guest runtime")
    set(REXGLUE_RUNTIME_RC_COPYRIGHT "(c) Skate3Recomp contributors")
endif()

include(cmake/RexGlue.cmake)
include(cmake/CodegenTargets.cmake)

set(SKATE3_COMMON_SOURCES
    src/exception_compat.cpp
    src/skate3_app_common.cpp
    src/skate3_online.cpp
    src/skate3_demo_path.cpp
    src/skate3_draw_distance.cpp
    src/skate3_fov.cpp
    src/skate3_iso_installer.cpp
    src/native/skate3_native_diagnostics.cpp
    src/native/skate3_native_entity.cpp
    src/native/skate3_native_guest_read.cpp
    src/native/skate3_native_lw.cpp
    src/native/skate3_native_palette.cpp
    src/skate3_native_debug_dialog.cpp
    src/skate3_native_render.cpp
    src/skate3_native_scene.cpp
    src/skate3_native_scene_gpu.cpp
    src/skate3_native_scene_post.cpp
    src/skate3_screenshot.cpp
    src/skate3_deck_shadow.cpp
    src/skate3_native_font_atlas.cpp
    src/generated/skate3_native_font_data.cpp
    # src/skate3_frustum_cull.cpp  # sk83_na_m native DoFrustumCulling replacement: dev-build LOD fields (cam+5248/5252/5256) retail does not have
    src/skate3_render_resolution.cpp
    src/skate3_settings_menu.cpp
    src/skate3_profiles.cpp
    src/skate3_splitscreen.cpp
    src/skate3_vr.cpp
    src/skate3_vr_guest_cull.cpp
    src/skate3_frustum_cull_retail.cpp  # retail native DoFrustumCulling (sub_82816A40)
    src/skate3_anim_twoconsec.cpp
    src/skate3_guest_aspect.cpp
    src/skate3_apt_spread.cpp
    src/skate3_shader_disasm.cpp
    src/skate3_title_update_installer.cpp
    src/skate3_user_settings.cpp
    src/skate3_win_icon.cpp
    src/skate3_window_icon.cpp
)
if(APPLE)
    list(APPEND SKATE3_COMMON_SOURCES src/skate3_iso_installer_macos.mm)
endif()

set(SKATE3_SOURCES
    src/main.cpp
    ${SKATE3_COMMON_SOURCES}
)

if(WIN32)
    add_executable(skate3 WIN32 ${SKATE3_SOURCES})
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/skate3_app.manifest
        ${CMAKE_CURRENT_BINARY_DIR}/skate3_app.manifest
        COPYONLY
    )
    target_link_options(skate3 PRIVATE LINKER:/MANIFESTINPUT:skate3_app.manifest)
    # Exe icon (Explorer + the SDK window class's MAINICON), the same way
    # sk83_na_m embeds its own. game/ is gitignored, so a checkout without
    # the file keeps the runtime fallback that derives the icon from the
    # game's dashboard art (skate3_win_icon.h).
    set(SKATE3_ICON_FILE ${CMAKE_CURRENT_SOURCE_DIR}/game/icon.ico)
    if(EXISTS ${SKATE3_ICON_FILE})
        set(SKATE3_RC_ICON_LINE "MAINICON ICON \"${SKATE3_ICON_FILE}\"")
        target_compile_definitions(skate3 PRIVATE SKATE3_EMBEDDED_ICON=1)
    else()
        set(SKATE3_RC_ICON_LINE "")
    endif()
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/skate3.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/skate3.rc
        @ONLY
    )
    target_sources(skate3 PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/skate3.rc)
    if(EXISTS ${SKATE3_ICON_FILE})
        set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/skate3.rc
            PROPERTIES OBJECT_DEPENDS ${SKATE3_ICON_FILE})
    endif()
else()
    add_executable(skate3 ${SKATE3_SOURCES})
endif()

target_include_directories(skate3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/include
)

set(SKATE3_NATIVE_SHADER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/scene.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/shadow_blur.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/resolve.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/blur.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/outline.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/overlay2d.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/spline.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/photo_fx.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/ssao.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/hbao.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/hdr.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/ssr.hlsl
)
# Shared .hlsli headers, inlined into the embedded sources by
# EmbedShaders.cmake: the embed must rerun when any of them changes.
file(GLOB SKATE3_NATIVE_SHADER_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/*.hlsli)
set(SKATE3_NATIVE_SHADER_HEADER
    ${CMAKE_CURRENT_BINARY_DIR}/include/skate3_native_shaders.h)
add_custom_command(
    OUTPUT ${SKATE3_NATIVE_SHADER_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DSHADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders
        -DOUT=${SKATE3_NATIVE_SHADER_HEADER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake
    DEPENDS ${SKATE3_NATIVE_SHADER_SOURCES}
        ${SKATE3_NATIVE_SHADER_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake
    COMMENT "Embedding native renderer shaders"
    VERBATIM)
target_sources(skate3 PRIVATE ${SKATE3_NATIVE_SHADER_HEADER})

# Vulkan SPIR-V of the native renderer shaders (tools/gen_native_spirv.py).
# The D3D12 backend compiles the HLSL at runtime; the Vulkan backend runs the
# offline-compiled SPIR-V in src/native/shaders/spirv. With Python and a
# SPIR-V-capable DXC (the Windows SDK's dxc.exe lacks SPIR-V codegen; use the
# GitHub release) the build regenerates it whenever a shader, a shared
# include or the generator changes; without them the checked-in header is
# used as is. `regen_native_spirv` forces a rebuild, e.g. after the renderer
# starts using a new shader entry point.
set(SKATE3_DXC ""
    CACHE FILEPATH "SPIR-V-capable dxc.exe for the native renderer's Vulkan shaders")
set(SKATE3_NATIVE_SPIRV_HEADER
    ${CMAKE_CURRENT_SOURCE_DIR}/src/native/shaders/spirv/skate3_native_shaders_spirv.h)
if(EXISTS "${SKATE3_DXC}" AND Python3_Interpreter_FOUND)
    set(SKATE3_NATIVE_SPIRV_COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_native_spirv.py
        --src ${CMAKE_CURRENT_SOURCE_DIR}/src --dxc ${SKATE3_DXC})
    add_custom_command(
        OUTPUT ${SKATE3_NATIVE_SPIRV_HEADER}
        COMMAND ${SKATE3_NATIVE_SPIRV_COMMAND}
        DEPENDS ${SKATE3_NATIVE_SHADER_SOURCES}
            ${SKATE3_NATIVE_SHADER_INCLUDES}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_native_spirv.py
        COMMENT "Compiling native renderer shaders to SPIR-V (Vulkan)"
        VERBATIM)
    add_custom_target(regen_native_spirv
        COMMAND ${SKATE3_NATIVE_SPIRV_COMMAND}
        COMMENT "Recompiling all native renderer shaders to SPIR-V (Vulkan)"
        VERBATIM)
    target_sources(skate3 PRIVATE ${SKATE3_NATIVE_SPIRV_HEADER})
    message(STATUS "skate3: Vulkan SPIR-V regenerated from HLSL with ${SKATE3_DXC}")
else()
    message(STATUS "skate3: no SPIR-V-capable DXC at ${SKATE3_DXC}; using the checked-in "
                   "Vulkan SPIR-V (stale once the HLSL changes)")
endif()
target_compile_definitions(skate3 PRIVATE SKATE3_BUILD_CONFIG="$<CONFIG>")
target_compile_definitions(skate3 PRIVATE
    SKATE3_HAS_TITLE_UPDATE=$<BOOL:${SKATE3_REQUIRE_TITLE_UPDATE}>)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/sources.cmake")
    include(generated/sources.cmake)
    target_sources(skate3 PRIVATE ${GENERATED_SOURCES})
    target_include_directories(skate3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/generated)
else()
    message(STATUS "generated/sources.cmake not found; run the generate-all target before building skate3")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/eawebkit/sources.cmake")
    set(_SKATE3_SAVED_GENERATED_SOURCES ${GENERATED_SOURCES})
    include(generated/eawebkit/sources.cmake)
    target_sources(skate3 PRIVATE ${GENERATED_SOURCES})
    target_include_directories(skate3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/generated/eawebkit)
    set(GENERATED_SOURCES ${_SKATE3_SAVED_GENERATED_SOURCES})
    unset(_SKATE3_SAVED_GENERATED_SOURCES)
else()
    message(STATUS "generated/eawebkit/sources.cmake not found; run the generate-all target before building skate3")
endif()

# The recompiler lowers the guest's multiply-add instructions as unfused
# add(mul(a,b), c) -- two roundings. The native animation decompressor must
# not be contracted into FMA (one rounding) or it drifts a few ulp from the
# guest (see skate3_anim_twoconsec.cpp).
set_source_files_properties(
    src/skate3_anim_twoconsec.cpp
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

# mimalloc as the process-wide allocator (Windows, dynamic CRT): mimalloc.dll
# built with MI_OVERRIDE plus the bundled mimalloc-redirect.dll patch the
# CRT's malloc/free for every module (exe, rexruntime.dll, drivers' CRT
# use), so no cross-module new/delete mismatch is possible. The renderer's
# per-frame containers and item copies make heap calls a measurable share
# of the guest render thread. Linked FIRST so mimalloc.dll heads the import
# table (mimalloc's load-order requirement); /INCLUDE keeps the otherwise
# unreferenced import. Build the DLL once with:
#   cmake -S _ext/mimalloc -B _ext/mimalloc/out/dll-release -G Ninja
#     -DCMAKE_BUILD_TYPE=Release -DMI_BUILD_SHARED=ON -DMI_BUILD_STATIC=OFF
#     -DMI_BUILD_OBJECT=OFF -DMI_BUILD_TESTS=OFF -DMI_OVERRIDE=ON
set(SKATE3_MIMALLOC_DIR ""
    CACHE PATH "Directory holding mimalloc.lib/.dll and mimalloc-redirect.dll")
option(SKATE3_MIMALLOC "Use mimalloc (process-wide malloc redirect) on Windows" ON)
if(WIN32 AND SKATE3_MIMALLOC AND NOT SKATE3_STATIC_MSVC_RUNTIME
   AND EXISTS "${SKATE3_MIMALLOC_DIR}/mimalloc.lib")
    target_link_libraries(skate3 PRIVATE "${SKATE3_MIMALLOC_DIR}/mimalloc.lib")
    target_link_options(skate3 PRIVATE "LINKER:/INCLUDE:mi_version")
    add_custom_command(TARGET skate3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SKATE3_MIMALLOC_DIR}/mimalloc.dll"
            "${SKATE3_MIMALLOC_DIR}/mimalloc-redirect.dll"
            "$<TARGET_FILE_DIR:skate3>"
        VERBATIM)
    message(STATUS "skate3: mimalloc from ${SKATE3_MIMALLOC_DIR}")
elseif(WIN32 AND SKATE3_MIMALLOC)
    message(STATUS "skate3: mimalloc not found in ${SKATE3_MIMALLOC_DIR}; using the CRT heap")
endif()

target_link_libraries(skate3 PRIVATE rex::runtime)
rexglue_configure_target(skate3)

# Optimisation settings, the same as sk83_na_m's: the generated PPC code is
# the bulk of every guest frame, and RelWithDebInfo is the build that gets
# played and profiled, so it optimises like Release (-O3, not CMake's -O2).
# ThinLTO lets the split recompiled units and the native renderer inline
# across translation units. The CPU target stays SKATE3_ENABLE_X86_64_V3's.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(skate3 PRIVATE
        -fomit-frame-pointer
        $<$<CONFIG:Release>:-O3 -DNDEBUG -funroll-loops>
        $<$<CONFIG:RelWithDebInfo>:-O3 -DNDEBUG -funroll-loops>)
    if(SKATE3_ENABLE_X86_64_V3 AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
        target_compile_options(skate3 PRIVATE -mprefer-vector-width=256)
    endif()
endif()
include(CheckIPOSupported)
check_ipo_supported(RESULT SKATE3_IPO_SUPPORTED OUTPUT SKATE3_IPO_ERROR)
if(SKATE3_IPO_SUPPORTED)
    set_property(TARGET skate3 PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set_property(TARGET skate3 PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
endif()

# Profile-guided optimisation (clang IR PGO). The recompiled PPC code is
# huge and branchy, so block layout and branch weights from a real play
# profile pay off across the whole guest frame.
#   GEN  - instrumented build: src/skate3_pgo.cpp dumps the counters to
#          SKATE3_PGO_PROFILE_DIR/skate3-<pid>.profraw every few seconds
#          (the game is usually killed, so the at-exit dump cannot be
#          relied on). Merge with llvm-profdata into SKATE3_PGO_PROFILE.
#   USE  - optimise with SKATE3_PGO_PROFILE (must exist).
#   AUTO - USE when SKATE3_PGO_PROFILE exists, otherwise a plain build.
#   OFF  - never.
# Functions edited since the profile was taken just lose their profile.
set(SKATE3_PGO "AUTO" CACHE STRING "Profile-guided optimisation: AUTO, GEN, USE or OFF")
set_property(CACHE SKATE3_PGO PROPERTY STRINGS AUTO GEN USE OFF)
set(SKATE3_PGO_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/pgo/skate3.profdata"
    CACHE FILEPATH "Merged PGO profile used by SKATE3_PGO=USE/AUTO")
set(SKATE3_PGO_PROFILE_DIR "${CMAKE_CURRENT_BINARY_DIR}/pgo-raw"
    CACHE PATH "Where a SKATE3_PGO=GEN build writes its .profraw files")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(SKATE3_PGO STREQUAL "GEN")
        execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                        OUTPUT_VARIABLE SKATE3_CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
        find_file(SKATE3_CLANG_PROFILE_RT NAMES clang_rt.profile-x86_64.lib clang_rt.profile.lib
                  PATHS "${SKATE3_CLANG_RESOURCE_DIR}/lib/windows"
                        "${SKATE3_CLANG_RESOURCE_DIR}/lib/x86_64-pc-windows-msvc"
                  NO_DEFAULT_PATH)
        if(NOT SKATE3_CLANG_PROFILE_RT)
            message(FATAL_ERROR "SKATE3_PGO=GEN: clang profile runtime not found under ${SKATE3_CLANG_RESOURCE_DIR}")
        endif()
        file(MAKE_DIRECTORY "${SKATE3_PGO_PROFILE_DIR}")
        target_compile_options(skate3 PRIVATE -fprofile-generate)
        target_sources(skate3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/skate3_pgo.cpp)
        target_compile_definitions(skate3 PRIVATE
            SKATE3_PGO_GEN "SKATE3_PGO_RAW_DIR=\"${SKATE3_PGO_PROFILE_DIR}\"")
        # The link runs with -nostdlib, so the driver does not add the runtime.
        target_link_libraries(skate3 PRIVATE "${SKATE3_CLANG_PROFILE_RT}")
        target_link_options(skate3 PRIVATE "LINKER:/INCLUDE:__llvm_profile_runtime")
        message(STATUS "skate3: PGO instrumented build (raw profiles -> ${SKATE3_PGO_PROFILE_DIR})")
    elseif(SKATE3_PGO STREQUAL "USE" OR (SKATE3_PGO STREQUAL "AUTO" AND EXISTS "${SKATE3_PGO_PROFILE}"))
        if(NOT EXISTS "${SKATE3_PGO_PROFILE}")
            message(FATAL_ERROR "SKATE3_PGO=USE: ${SKATE3_PGO_PROFILE} does not exist")
        endif()
        target_compile_options(skate3 PRIVATE
            "-fprofile-use=${SKATE3_PGO_PROFILE}"
            -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-profile-instr-missing
            -Wno-backend-plugin)
        # Rebuild everything when the profile changes.
        set_property(SOURCE ${SKATE3_SOURCES} ${GENERATED_SOURCES} APPEND PROPERTY
                     OBJECT_DEPENDS "${SKATE3_PGO_PROFILE}")
        message(STATUS "skate3: PGO using ${SKATE3_PGO_PROFILE}")
    endif()
endif()

# Release strips the investigation probes (per-draw pass census and ring,
# logging-only hooks), as in sk83_na_m; see SK83_RELEASE_BUILD in src/.
target_compile_definitions(skate3 PRIVATE $<$<CONFIG:Release>:SK83_RELEASE_BUILD>)

if(APPLE)
    target_link_options(skate3 PRIVATE -Wl,-stack_size,0x4000000)
    target_link_libraries(skate3 PRIVATE "-framework Cocoa")
endif()

if(WIN32)
    target_link_libraries(skate3 PRIVATE comdlg32 winhttp d3dcompiler gdiplus)
endif()

# SteamVR output (src/skate3_vr.cpp): the OpenVR C API header, and the
# runtime DLL staged next to the exe (loaded on demand; without it, or
# without SteamVR, the game simply runs flat).
target_include_directories(skate3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty)
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/openvr/openvr_api.dll")
    add_custom_command(TARGET skate3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/openvr/openvr_api.dll"
            "$<TARGET_FILE_DIR:skate3>/openvr_api.dll"
        VERBATIM)
endif()

if(NOT SKATE3_EFFECTIVE_TITLE_UPDATE_PACKAGE STREQUAL "")
    add_custom_target(skate3_stage_title_update_patches
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:skate3>/game/data/webkit"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SKATE3_TITLE_UPDATE_DEFAULT_XEXP}"
            "$<TARGET_FILE_DIR:skate3>/game/default.xexp"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SKATE3_TITLE_UPDATE_EAWEBKIT_XEXP}"
            "$<TARGET_FILE_DIR:skate3>/game/data/webkit/EAWebkit.xexp"
        DEPENDS skate3-title-update-codegen-inputs
        VERBATIM
    )
    add_dependencies(skate3 skate3_stage_title_update_patches)
endif()

if(MSVC)
    target_compile_options(skate3 PRIVATE /utf-8)
else()
    target_compile_options(skate3 PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
endif()
