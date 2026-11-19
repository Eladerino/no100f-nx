# The Nintendo Switch (Horizon homebrew) layer of FrightWorks' WiiCompiled runtime build.
# Adapted from spm-nx switch/cmake/SpmSwitch.cmake (12ec5e7); names follow FrightWorks' tree
# (RECOMP_*, recomp_*, ModProduct).
#
# wiicompiled/runtime/CMakeLists.txt includes this right after it recognises the devkitPro Switch
# toolchain (CMAKE_SYSTEM_NAME NintendoSwitch), and calls the fw_switch_* functions below at the few
# points where the Switch differs. Everything Switch-specific lives under switch/; the upstream tree
# only carries those hook calls and a handful of `__SWITCH__` branches.
#
# Compilers: devkitA64's GCC stays CMake's compiler, so it drives every link with libnx's specs. Dawn
# (with abseil) and the SDL 3 shim are configured first and compile with GCC. Every target created
# after this file (Crypto++, pugixml, Aurora and its extern libraries, the runtime, the translated
# game) compiles with clang through cmake/clang-launcher.sh, because WiiCompiled only supports Clang.
# Both use devkitA64's newlib and libstdc++ headers, so the objects link together.
#
# Milestones: H2 (this file's default) is the null-backend boot: Dawn's Null device, no surface, no
# deko3d. FW_SWITCH_DEKO=ON is H3: the deko3d interposer (switch/deko, ported from spm-nx) shadows
# Aurora's wgpu calls into deko3d and presents through its swapchain.
#
# FW_SWITCH_AUDIT=ON with RECOMP_BUILD_PRODUCTS=OFF skips Dawn, the SDL shim and Aurora and compiles
# just the runtime's own sources for the Switch (recomp_switch_audit): no network, no translated game.

cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH FW_SWITCH_DIR)
set(RECOMP_RUNTIME_DIR_FOR_SWITCH "${CMAKE_CURRENT_SOURCE_DIR}")

option(FW_SWITCH_AUDIT "Switch: compile only the runtime's sources (no Dawn, SDL shim or Aurora)" OFF)
option(FW_SWITCH_DEKO "Switch: deko3d renderer through the wgpu interposer (H3)" OFF)
option(FW_SWITCH_DEBUG_SERVER "Switch: start the debug server (TCP 6543, no password) at boot" ON)
option(FW_SWITCH_CHECKED_GUEST_MEMORY "Switch: check every guest memory access (no flat window)" OFF)

# Instrumented-PGO capture build (ported from spm-nx's spec E wiring): the translated shards
# (and, with FW_PGO_ALL, the runtime and Aurora too) count every block they run, and
# switch/source/pgo_instr_switch.c writes the counts to UserData/pgo. A capture build only:
# its frame rates are never quoted.
option(FW_PGO_INSTRUMENT "Switch: instrumented PGO capture build (counts to UserData/pgo)" OFF)
option(FW_PGO_ALL "Switch: with FW_PGO_INSTRUMENT, also instrument the runtime and Aurora" OFF)
option(FW_PGO_TRIM_AURORA "Switch: with FW_PGO_ALL, trim the Aurora scope to the spm-nx seven targets (no aurora_gd/os/ms/card)" OFF)
# The other half: a build that consumes a trained profile. FW_PGO_PROFILE is the merged
# .profdata (llvm-profdata output over the capture build's .profraw files), given as a path
# the build machine sees; build-switch.sh's pgo-use variant merges into the build tree and
# passes /work/pgo/no100f.profdata. The profile's manifest.txt (written by the capture build
# into the repo's pgo/ folder: region, tree commit, clang major, scope, shards fingerprint)
# must sit in the same directory: a profile with no provenance is refused. Function-level
# staleness inside the profile is clang's per-function hash job; build-switch.sh counts its
# out-of-date warnings after the build and fails the variant when too much of the profile no
# longer fits (see pgo-fit-check.py).
option(FW_PGO_USE "Switch: build with a trained PGO profile (FW_PGO_PROFILE)" OFF)
set(FW_PGO_PROFILE "" CACHE FILEPATH
    "Switch: merged .profdata for FW_PGO_USE, with its manifest.txt beside it")
if(FW_PGO_USE AND FW_SWITCH_AUDIT)
    message(FATAL_ERROR
        "no100f-nx: FW_PGO_USE with FW_SWITCH_AUDIT compiles nothing the profile covers; "
        "the audit target takes no PGO flags. Build a product variant instead.")
endif()
if(FW_PGO_USE AND FW_PGO_INSTRUMENT)
    message(FATAL_ERROR
        "no100f-nx: FW_PGO_USE and FW_PGO_INSTRUMENT are opposite builds (one consumes a "
        "profile, the other records one); enable exactly one.")
endif()
# The NRO, its ELF and its SD folder (sdmc:/switch/no100f-nx/), named after the clone like spm-nx.
set(FW_SWITCH_NRO_NAME "no100f-nx" CACHE STRING "Switch: file name of the NRO, its ELF and its SD folder")
set(FW_SWITCH_SDL3_HEADERS "" CACHE PATH
    "Switch: an SDL 3.4.4 source tree for its headers (empty: download the pinned release)")
set(FW_SWITCH_IMGUI_DIR "" CACHE PATH
    "Switch audit only: an ImGui v1.91.9b-docking tree for its headers (empty: download it)")
set(FW_SWITCH_VERSION "0.0.1" CACHE STRING "Switch: NACP version")

set(DEVKITPRO_ROOT "$ENV{DEVKITPRO}" CACHE PATH "devkitPro installation root")
if(NOT DEVKITPRO_ROOT)
    set(DEVKITPRO_ROOT "/opt/devkitpro")
endif()
set(SWITCH_PORTLIBS "${DEVKITPRO_ROOT}/portlibs/switch")

include(FetchContent)
# HostContext's context switch is assembly (fw_switch_runtime_sources); enable_language only works
# at directory scope, so here rather than in the function.
enable_language(ASM)

# ---- SDL 3 headers (Aurora's pin) ---------------------------------------------------------------
if(FW_SWITCH_SDL3_HEADERS)
    set(FW_SWITCH_SDL3_INCLUDE "${FW_SWITCH_SDL3_HEADERS}/include")
else()
    FetchContent_Declare(fw_sdl3_headers
        URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.4.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR do-not-build)
    FetchContent_MakeAvailable(fw_sdl3_headers)
    set(FW_SWITCH_SDL3_INCLUDE "${fw_sdl3_headers_SOURCE_DIR}/include")
endif()

if(NOT FW_SWITCH_AUDIT)
    # ---- Dawn: Null backend + Tint's GLSL writer, patched for Horizon (switch/dawn) --------------
    # CMake's try_compile runs GCC without the clang launcher, so Dawn's C++20-module probe passes
    # on GCC and then hands GCC's -fmodules-ts flags to clang. Nothing uses dawn::cpp_module.
    set(DAWN_SUPPORTS_CXX_MODULES OFF CACHE BOOL "" FORCE)
    include("${FW_SWITCH_DIR}/dawn/dawn.cmake")

    # ---- deko3d renderer (switch/deko): takes Aurora's wgpu calls over at link time (H3) -------
    # Vendored deko3d 0.5.0 and uam 1.1.0 build from their own CMakeLists, then the interposer as
    # fw_deko. Still GCC scope like Dawn above: the clang launcher is only set further down.
    # uam's CMakeLists keys its Switch path (no Threads package, no host tool) off NX_ROOT.
    # Mirrors spm-nx's SpmSwitch.cmake block of the same shape.
    if(FW_SWITCH_DEKO)
        if(NOT DEFINED NX_ROOT)
            set(NX_ROOT "${DEVKITPRO_ROOT}")
        endif()
        add_subdirectory("${FW_SWITCH_DIR}/deko3d" "${CMAKE_BINARY_DIR}/fw_deko3d")
        add_subdirectory("${FW_SWITCH_DIR}/uam" "${CMAKE_BINARY_DIR}/fw_uam")
        file(GLOB _fw_deko_sources CONFIGURE_DEPENDS "${FW_SWITCH_DIR}/deko/*.cpp")
        add_library(fw_deko STATIC ${_fw_deko_sources})
        target_compile_features(fw_deko PUBLIC cxx_std_20)
        # Dawn's own tree for the "src/tint/..." includes in the shader translation; the generated
        # webgpu headers land in Dawn's binary dir (the tint targets also carry these transitively).
        target_include_directories(fw_deko PRIVATE
            "${dawn_SOURCE_DIR}" "${dawn_BINARY_DIR}/gen/include" "${dawn_BINARY_DIR}/gen")
        target_link_libraries(fw_deko PUBLIC
            deko3d_release uamlib
            tint_api tint_lang_glsl_writer tint_lang_wgsl_reader tint_lang_wgsl_inspector
            nx)
        # fw_deko compiles against Dawn's generated headers (dawn/webgpu.h); without an explicit
        # dependency its objects can compile before the generator runs (the first deko build raced
        # exactly there). The tint link dependency alone does not order it.
        if(TARGET webgpu_dawn)
            add_dependencies(fw_deko webgpu_dawn)
        elseif(TARGET dawn_headers)
            add_dependencies(fw_deko dawn_headers)
        endif()
    endif()

    # ---- The SDL 3 API on libnx (switch/sdl3_shim), standing in for SDL3-static ------------------
    file(GLOB _fw_shim_sources CONFIGURE_DEPENDS "${FW_SWITCH_DIR}/sdl3_shim/*.c")
    add_library(SDL3-static STATIC ${_fw_shim_sources})
    add_library(SDL3::SDL3-static ALIAS SDL3-static)
    target_include_directories(SDL3-static PUBLIC "${FW_SWITCH_SDL3_INCLUDE}")
    target_include_directories(SDL3-static PRIVATE "${FW_SWITCH_DIR}/sdl3_shim")
    target_link_libraries(SDL3-static PUBLIC nx)

    # ---- Tracy: headers only (TRACY_ENABLE stays off) ---------------------------------------------
    FetchContent_Declare(tracy
        URL https://github.com/wolfpld/tracy/archive/a64b9a20294d59421a2f57aeca3c6383d8c48169.tar.gz
        URL_HASH SHA256=24d342b5127d7f659dc3cf94f24347b348cc736f25b17a691fd7f69541937658
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        SOURCE_SUBDIR do-not-build)
    FetchContent_MakeAvailable(tracy)
    add_library(TracyClient INTERFACE)
    target_include_directories(TracyClient INTERFACE "${tracy_SOURCE_DIR}/public")
    add_library(Tracy::TracyClient ALIAS TracyClient)

    set(AURORA_SDL3_PROVIDER vendor CACHE STRING "" FORCE)
    set(AURORA_SDL3_LINKAGE static CACHE STRING "" FORCE)
    set(AURORA_DAWN_PROVIDER vendor CACHE STRING "" FORCE)
    set(AURORA_DAWN_LINKAGE static CACHE STRING "" FORCE)
    set(AURORA_CACHE_USE_ZSTD OFF CACHE BOOL "" FORCE)
    set(AURORA_SDL3_LIBUSB OFF CACHE BOOL "" FORCE)
    set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
endif()

set(MBEDTLS_USER_CONFIG_FILE "${FW_SWITCH_DIR}/include/spm_mbedtls_config.h" CACHE FILEPATH "" FORCE)

# From here on everything compiles with clang (see the top of this file).
set(CMAKE_C_COMPILER_LAUNCHER "${FW_SWITCH_DIR}/cmake/clang-launcher.sh")
set(CMAKE_CXX_COMPILER_LAUNCHER "${FW_SWITCH_DIR}/cmake/clang-launcher.sh")
add_compile_definitions(SPM_SWITCH_LAUNCHER_REV=2)

if(FW_SWITCH_CHECKED_GUEST_MEMORY)
    add_compile_definitions(RECOMP_SWITCH_CHECKED_GUEST_MEMORY=1)
endif()

macro(fw_switch_dawn_backends)
    set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_METAL OFF CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_NULL ON CACHE BOOL "" FORCE)
    set(TINT_BUILD_HLSL_WRITER OFF CACHE BOOL "" FORCE)
endmacro()

# After Aurora is configured: directory-scoped includes and the Horizon swaps in Aurora's extern libs.
macro(fw_switch_enter_project_scope)
    include_directories(BEFORE SYSTEM "${FW_SWITCH_DIR}/include")
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-include ${FW_SWITCH_DIR}/include/spm_switch_compat.h>")
    if(TARGET aurora_core)
        foreach(_fw_t aurora_core aurora_gx aurora_gd aurora_os aurora_pad aurora_si aurora_ms aurora_mtx
                      aurora_vi aurora_main aurora_card)
            if(TARGET ${_fw_t})
                target_include_directories(${_fw_t} SYSTEM BEFORE PRIVATE "${FW_SWITCH_DIR}/include")
                target_include_directories(${_fw_t} PRIVATE "${SWITCH_PORTLIBS}/include")
                target_include_directories(${_fw_t} PRIVATE "${FW_SWITCH_DIR}")
                target_compile_options(${_fw_t} PRIVATE
                    "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-include ${FW_SWITCH_DIR}/include/spm_switch_compat.h>")
                if(FW_SWITCH_DEKO)
                    target_compile_definitions(${_fw_t} PRIVATE FW_SWITCH_DEKO=1)
                endif()
            endif()
        endforeach()
        target_link_libraries(aurora_core PUBLIC nx)
    endif()
    if(TARGET sqlite3)
        target_compile_definitions(sqlite3 PRIVATE SQLITE_OMIT_WAL=1 SQLITE_MAX_MMAP_SIZE=0
            SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_THREADSAFE=1 SQLITE_DEFAULT_LOCKING_MODE=1
            SQLITE_TEMP_STORE=3)
        target_sources(sqlite3 PRIVATE "${FW_SWITCH_DIR}/aurora/sqlite_horizon.c")
        get_target_property(_fw_sqlite_sources sqlite3 SOURCES)
        list(GET _fw_sqlite_sources 0 _fw_sqlite_amalgamation)
        get_filename_component(_fw_sqlite_dir "${_fw_sqlite_amalgamation}" DIRECTORY)
        target_include_directories(sqlite3 PRIVATE "${_fw_sqlite_dir}")
    endif()
    if(TARGET imgui_backends)
        # ImGui's desktop SDL backends become the Horizon one. FrightWorks also wraps
        # imgui_impl_sdl3.cpp in lib/imgui_sdl_owner.cpp (#include IMGUI_SDL_OWNER_SOURCE, to relabel
        # the menu buttons), so that wrapper goes too or the whole desktop backend comes back.
        get_target_property(_fw_imgui_backends imgui_backends SOURCES)
        list(FILTER _fw_imgui_backends EXCLUDE REGEX "imgui_impl_sdl(renderer)?3\\.cpp$")
        list(FILTER _fw_imgui_backends EXCLUDE REGEX "imgui_sdl_owner\\.cpp$")
        list(APPEND _fw_imgui_backends "${FW_SWITCH_DIR}/aurora/imgui_sdl_backends_horizon.cpp")
        set_property(TARGET imgui_backends PROPERTY SOURCES ${_fw_imgui_backends})
    endif()
    if(TARGET imgui)
        target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
    endif()
endmacro()

# The runtime's sources for Horizon: the flat guest memory and the context switch swapped for their
# Horizon versions, plus the process setup, the POSIX gaps newlib leaves, and the debug server.
function(fw_switch_runtime_sources list_var)
    set(_sources ${${list_var}})
    list(REMOVE_ITEM _sources
        "${RECOMP_RUNTIME_DIR_FOR_SWITCH}/src/guest_flat_memory.cpp"
        "${RECOMP_RUNTIME_DIR_FOR_SWITCH}/src/guest_flat_memory_macos.cpp")
    set(_shims
        "${FW_SWITCH_DIR}/runtime/horizon_flat_base.c"
        "${FW_SWITCH_DIR}/runtime/horizon_mman.c"
        "${FW_SWITCH_DIR}/runtime/horizon_net.c"
        "${FW_SWITCH_DIR}/runtime/horizon_mbedtls.c"
        "${FW_SWITCH_DIR}/runtime/horizon_unistd.c"
        "${FW_SWITCH_DIR}/runtime/horizon_pthread.c"
        "${FW_SWITCH_DIR}/runtime/horizon_tls.c")
    list(APPEND _sources
        "${RECOMP_RUNTIME_DIR_FOR_SWITCH}/src/platform/android/co_switch.S"
        "${FW_SWITCH_DIR}/runtime/guest_flat_memory_horizon.cpp"
        ${_shims}
        "${FW_SWITCH_DIR}/source/fw_switch_init.cpp"
        "${FW_SWITCH_DIR}/source/debug_server.cpp"
        "${FW_SWITCH_DIR}/source/fw_debug_server.cpp")
    set_source_files_properties(${_shims}
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON SKIP_UNITY_BUILD_INCLUSION ON)
    set_source_files_properties(
        "${FW_SWITCH_DIR}/runtime/guest_flat_memory_horizon.cpp"
        "${FW_SWITCH_DIR}/source/fw_switch_init.cpp"
        "${FW_SWITCH_DIR}/source/debug_server.cpp"
        "${FW_SWITCH_DIR}/source/fw_debug_server.cpp"
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    if(FW_SWITCH_DEBUG_SERVER)
        set(_server 1)
    else()
        set(_server 0)
    endif()
    set_source_files_properties("${FW_SWITCH_DIR}/source/fw_switch_init.cpp" PROPERTIES COMPILE_DEFINITIONS
        "FW_SWITCH_ROOT=\"/switch/${FW_SWITCH_NRO_NAME}\";FW_SWITCH_ELF_NAME=\"${FW_SWITCH_NRO_NAME}.elf\";FW_SWITCH_DEBUG_SERVER=${_server}")
    set(${list_var} ${_sources} PARENT_SCOPE)
endfunction()

# RECOMP_BUILD_PRODUCTS=OFF with FW_SWITCH_AUDIT=ON: the runtime's sources as one object library.
function(fw_switch_add_audit_target sources)
    set(_audit ${sources})
    # These need the translator's generated/RuntimeConfig.h, which only a translated build has.
    list(REMOVE_ITEM _audit
        "${RECOMP_RUNTIME_DIR_FOR_SWITCH}/src/abi_bridge.cpp"
        "${RECOMP_RUNTIME_DIR_FOR_SWITCH}/src/hle/os/os_alarm.cpp")
    if(FW_SWITCH_IMGUI_DIR)
        set(_imgui "${FW_SWITCH_IMGUI_DIR}")
    else()
        FetchContent_Declare(fw_imgui_headers
            URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.tar.gz
            URL_HASH SHA256=466fdef9b18de15f0bb6e288e3d00ffa3d82200ec458ce5e4f724a161d9528a5
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE
            SOURCE_SUBDIR do-not-build)
        FetchContent_MakeAvailable(fw_imgui_headers)
        set(_imgui "${fw_imgui_headers_SOURCE_DIR}")
    endif()
    add_library(recomp_switch_audit OBJECT ${_audit})
    target_include_directories(recomp_switch_audit BEFORE PRIVATE "${FW_SWITCH_DIR}/include")
    target_include_directories(recomp_switch_audit PRIVATE
        "${RECOMP_RUNTIME_DIR_FOR_SWITCH}/include" "${RECOMP_RUNTIME_DIR_FOR_SWITCH}/src"
        "${RECOMP_RUNTIME_DIR_FOR_SWITCH}/.." "${RECOMP_RUNTIME_DIR_FOR_SWITCH}/../aurora-main/include"
        "${FW_SWITCH_DIR}/source")
    target_include_directories(recomp_switch_audit SYSTEM PRIVATE
        "${FW_SWITCH_SDL3_INCLUDE}" "${_imgui}" "${SWITCH_PORTLIBS}/include")
    target_compile_features(recomp_switch_audit PRIVATE cxx_std_20)
    target_compile_definitions(recomp_switch_audit PRIVATE SDL_MAIN_HANDLED TARGET_PC)
    target_link_libraries(recomp_switch_audit PRIVATE
        recomp::pugixml recomp::toml11 recomp::cryptopp recomp::mbedtls)
    set_target_properties(recomp_switch_audit PROPERTIES UNITY_BUILD OFF)
endfunction()

# After cmake/PublicProducts.cmake: the ELF, its link wraps and the NRO.
function(fw_switch_configure_products)
    if(NOT TARGET WiiCompiled)
        return()
    endif()
    if(TARGET recomp_runtime_common)
        target_include_directories(recomp_runtime_common PRIVATE "${FW_SWITCH_DIR}/source")
        target_include_directories(recomp_runtime_common SYSTEM PRIVATE "${SWITCH_PORTLIBS}/include")
    endif()
    # Instrumented PGO (flags verbatim from spm-nx's SpmSwitch.cmake, which took them from
    # KartPadNX's capture build on this same toolchain family): counter promotion stays off,
    # it goes superlinear on translated loop nests. Scope from day one is the whole clang
    # scope (FW_PGO_ALL, and build-switch.sh's pgo variant always sets it): SwitchWakerHD
    # priced a game-only profile at +0.11 fps against +1.95 for game+runtime. Never fw_deko,
    # Dawn, deko3d or uam: they compile in GCC scope before the clang launcher is set and
    # cannot take Clang PGO.
    if(FW_PGO_INSTRUMENT)
        # Language-gated (COMPILE_LANGUAGE C,CXX): the -mllvm options are Clang-only, and
        # targets in scope carry .S sources that the GCC assembler driver compiles.
        set(_fw_pgo_flags
            "$<$<COMPILE_LANGUAGE:C,CXX>:-fprofile-generate>"
            "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-mllvm -disable-vp=true>"
            "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-mllvm -static-func-full-module-prefix=false>"
            "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-mllvm -do-counter-promotion=false>")
        foreach(_fw_pgo_target IN ITEMS
                recomp_base_shared recomp_base_sensitive recomp_mod_sensitive recomp_mod_functions)
            if(TARGET ${_fw_pgo_target})
                target_compile_options(${_fw_pgo_target} PRIVATE ${_fw_pgo_flags})
            endif()
        endforeach()
        if(FW_PGO_ALL)
            # FW_PGO_TRIM_AURORA narrows the Aurora list to the spm-nx SPM_PGO_ALL scope
            # (wedge-diagnosis step 3: aurora_gd/os/ms/card host the card/OS corridor the
            # capture build wedges on; the trim tests whether their instrumentation cost
            # is what starves the poll-driven completion drain).
            set(_fw_pgo_aurora
                aurora_core aurora_gx aurora_gd aurora_os aurora_pad aurora_si aurora_ms
                aurora_mtx aurora_vi aurora_main aurora_card)
            if(FW_PGO_TRIM_AURORA)
                list(REMOVE_ITEM _fw_pgo_aurora aurora_gd aurora_os aurora_ms aurora_card)
            endif()
            foreach(_fw_pgo_target IN ITEMS
                    recomp_runtime_common WiiCompiled ModProduct ${_fw_pgo_aurora})
                if(TARGET ${_fw_pgo_target})
                    target_compile_options(${_fw_pgo_target} PRIVATE ${_fw_pgo_flags})
                endif()
            endforeach()
        endif()
        target_sources(WiiCompiled PRIVATE "${FW_SWITCH_DIR}/source/pgo_instr_switch.c")
        set_source_files_properties("${FW_SWITCH_DIR}/source/pgo_instr_switch.c"
            PROPERTIES SKIP_PRECOMPILE_HEADERS ON SKIP_UNITY_BUILD_INCLUSION ON)
        message(STATUS "no100f-nx: instrumented PGO capture build (FW_PGO_ALL=${FW_PGO_ALL} FW_PGO_TRIM_AURORA=${FW_PGO_TRIM_AURORA})")
    endif()
    # PGO use (the consuming half): the trained profile applies to the same scope the capture
    # build records (shards + runtime + Aurora, always: a game-only profile is the measured
    # ~0-gain shape, so there is no FW_PGO_ALL split on this side and the manifest check below
    # refuses a profile trained without the runtime half). The two -mllvm options match the
    # capture side so function names and the (absent) value profiles line up with what the
    # writer recorded. Fit is checked here at the provenance level (manifest beside the
    # profile: right region, right scope, recorded at all) and in build-switch.sh after the
    # build at the function level (pgo-fit-check.py).
    if(FW_PGO_USE)
        if(NOT FW_PGO_PROFILE)
            message(FATAL_ERROR
                "no100f-nx: FW_PGO_USE needs FW_PGO_PROFILE=<merged .profdata>; "
                "build-switch.sh --variant pgo-use sets it after merging the raw profiles.")
        endif()
        if(NOT EXISTS "${FW_PGO_PROFILE}")
            message(FATAL_ERROR
                "no100f-nx: FW_PGO_PROFILE does not exist: ${FW_PGO_PROFILE}. Merge the "
                "capture build's .profraw files first (build-switch.sh --variant pgo-use "
                "does this from the repo's pgo/ folder).")
        endif()
        cmake_path(GET FW_PGO_PROFILE PARENT_PATH _fw_pgo_dir)
        set(_fw_pgo_manifest "${_fw_pgo_dir}/manifest.txt")
        if(NOT EXISTS "${_fw_pgo_manifest}")
            message(FATAL_ERROR
                "no100f-nx: no manifest.txt beside the profile (${_fw_pgo_manifest}). A "
                "profile without provenance is not used: it could belong to another tree, "
                "region or compiler and clang would apply only the parts that still hash, "
                "silently. The --variant pgo capture build writes the manifest into the "
                "repo's pgo/ folder; keep it with the raw profiles.")
        endif()
        file(READ "${_fw_pgo_manifest}" _fw_pgo_manifest_text)
        foreach(_fw_pgo_key IN ITEMS region commit clang_major scope)
            string(REGEX MATCH "(^|\n)${_fw_pgo_key}=([^\r\n]*)" _fw_pgo_m
                "${_fw_pgo_manifest_text}")
            if(NOT CMAKE_MATCH_2)
                message(FATAL_ERROR
                    "no100f-nx: the profile manifest (${_fw_pgo_manifest}) has no "
                    "${_fw_pgo_key}= line; it is not a manifest this build can check. "
                    "Re-run the --variant pgo capture build to regenerate it.")
            endif()
            set(_fw_pgo_${_fw_pgo_key} "${CMAKE_MATCH_2}")
        endforeach()
        if(NOT _fw_pgo_region STREQUAL "GIHE78-USA")
            message(FATAL_ERROR
                "no100f-nx: the profile was trained for region '${_fw_pgo_region}', but the "
                "Switch build is the USA game (GIHE78) only: generated code differs per "
                "region, so this profile cannot fit. Train a USA profile.")
        endif()
        if(NOT _fw_pgo_scope MATCHES "game\\+runtime\\+aurora")
            message(FATAL_ERROR
                "no100f-nx: the profile's scope is '${_fw_pgo_scope}', not "
                "game+runtime+aurora. A game-only profile is the shape SwitchWakerHD "
                "measured at +0.11 fps against +1.95 for game+runtime; refusing it rather "
                "than shipping a PGO build that cannot gain. Re-train with FW_PGO_ALL.")
        endif()
        if(NOT _fw_pgo_clang_major STREQUAL "19")
            message(FATAL_ERROR
                "no100f-nx: the profile was recorded with clang major "
                "'${_fw_pgo_clang_major}', but this build's toolchain is clang 19 "
                "(switch/scripts/Containerfile): raw profile formats are version-locked. "
                "Re-train with the current toolchain.")
        endif()
        # A retrained profile lands at the same path, so the flags above would not change and
        # ninja would relink old objects built from the previous profile. The profile is a
        # configure dependency, and its hash rides on every PGO compile line as a define, so
        # new profile contents recompile everything it covers and the same contents recompile
        # nothing.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FW_PGO_PROFILE}")
        file(SHA256 "${FW_PGO_PROFILE}" _fw_pgo_profile_sha)
        string(SUBSTRING "${_fw_pgo_profile_sha}" 0 16 _fw_pgo_profile_sha)
        set(_fw_pgo_use_flags
            "$<$<COMPILE_LANGUAGE:C,CXX>:-fprofile-instr-use=${FW_PGO_PROFILE}>"
            "$<$<COMPILE_LANGUAGE:C,CXX>:-DFW_PGO_PROFILE_SHA=0x${_fw_pgo_profile_sha}ull>"
            "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-mllvm -disable-vp=true>"
            "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-mllvm -static-func-full-module-prefix=false>")
        foreach(_fw_pgo_target IN ITEMS
                recomp_base_shared recomp_base_sensitive recomp_mod_sensitive recomp_mod_functions
                recomp_runtime_common WiiCompiled ModProduct
                aurora_core aurora_gx aurora_gd aurora_os aurora_pad aurora_si aurora_ms
                aurora_mtx aurora_vi aurora_main aurora_card)
            if(TARGET ${_fw_pgo_target})
                target_compile_options(${_fw_pgo_target} PRIVATE ${_fw_pgo_use_flags})
            endif()
        endforeach()
        message(STATUS
            "no100f-nx: PGO USE build: profile ${FW_PGO_PROFILE} sha ${_fw_pgo_profile_sha} (trained on tree "
            "${_fw_pgo_commit}, clang ${_fw_pgo_clang_major}, scope ${_fw_pgo_scope}); "
            "build-switch.sh enforces the function-level fit check after the build")
    endif()
    set_target_properties(WiiCompiled PROPERTIES OUTPUT_NAME "${FW_SWITCH_NRO_NAME}" SUFFIX ".elf")
    target_link_libraries(WiiCompiled PRIVATE SDL3::SDL3-static nx)
    if(FW_SWITCH_DEKO)
        target_link_libraries(WiiCompiled PRIVATE fw_deko deko3d_release)
        # The renderer takes over Aurora's WebGPU calls at link time: every __wrap_wgpu* defined
        # in switch/deko/*.cpp needs its --wrap here, or that call silently goes to Dawn only.
        # This list is generated, not typed:
        #   grep -ho '__wrap_wgpu[A-Za-z]*' switch/deko/*.cpp | sort -u | sed 's/__wrap_//'
        # (64 functions at the H3 port; re-run it when switch/deko changes.)
        set(_fw_dk_wraps
        wgpuBindGroupAddRef
        wgpuBindGroupLayoutAddRef
        wgpuBindGroupLayoutRelease
        wgpuBindGroupRelease
        wgpuBufferAddRef
        wgpuBufferGetConstMappedRange
        wgpuBufferGetMappedRange
        wgpuBufferMapAsync
        wgpuBufferRelease
        wgpuCommandBufferAddRef
        wgpuCommandBufferRelease
        wgpuCommandEncoderAddRef
        wgpuCommandEncoderBeginComputePass
        wgpuCommandEncoderBeginRenderPass
        wgpuCommandEncoderCopyBufferToBuffer
        wgpuCommandEncoderCopyBufferToTexture
        wgpuCommandEncoderCopyTextureToBuffer
        wgpuCommandEncoderCopyTextureToTexture
        wgpuCommandEncoderFinish
        wgpuCommandEncoderRelease
        wgpuComputePassEncoderAddRef
        wgpuComputePassEncoderDispatchWorkgroups
        wgpuComputePassEncoderEnd
        wgpuComputePassEncoderRelease
        wgpuComputePassEncoderSetBindGroup
        wgpuComputePassEncoderSetPipeline
        wgpuComputePipelineAddRef
        wgpuComputePipelineRelease
        wgpuDeviceCreateBindGroup
        wgpuDeviceCreateBindGroupLayout
        wgpuDeviceCreateBuffer
        wgpuDeviceCreateCommandEncoder
        wgpuDeviceCreateComputePipeline
        wgpuDeviceCreateRenderPipeline
        wgpuDeviceCreateSampler
        wgpuDeviceCreateShaderModule
        wgpuDeviceCreateTexture
        wgpuQueueSubmit
        wgpuQueueWriteBuffer
        wgpuQueueWriteTexture
        wgpuRenderPassEncoderAddRef
        wgpuRenderPassEncoderDraw
        wgpuRenderPassEncoderDrawIndexed
        wgpuRenderPassEncoderEnd
        wgpuRenderPassEncoderRelease
        wgpuRenderPassEncoderSetBindGroup
        wgpuRenderPassEncoderSetBlendConstant
        wgpuRenderPassEncoderSetIndexBuffer
        wgpuRenderPassEncoderSetPipeline
        wgpuRenderPassEncoderSetScissorRect
        wgpuRenderPassEncoderSetStencilReference
        wgpuRenderPassEncoderSetVertexBuffer
        wgpuRenderPassEncoderSetViewport
        wgpuRenderPipelineAddRef
        wgpuRenderPipelineRelease
        wgpuSamplerAddRef
        wgpuSamplerRelease
        wgpuShaderModuleAddRef
        wgpuShaderModuleRelease
        wgpuTextureAddRef
        wgpuTextureCreateView
        wgpuTextureRelease
        wgpuTextureViewAddRef
        wgpuTextureViewRelease)
        foreach(_fw_dk_fn IN LISTS _fw_dk_wraps)
            target_link_options(WiiCompiled PRIVATE "LINKER:--wrap=${_fw_dk_fn}")
        endforeach()
    endif()
    # Every pthread and std::thread gets a 4 MiB stack and the application cores, and detaching one
    # works (runtime/horizon_pthread.c): libnx's default 128 KiB overflows in Tint, and libnx has no
    # working pthread_detach. rename() replaces an existing file the way POSIX does (horizon_unistd.c).
    target_link_options(WiiCompiled PRIVATE "LINKER:--wrap=pthread_create" "LINKER:--wrap=pthread_detach")
    target_link_options(WiiCompiled PRIVATE "LINKER:--wrap=threadCreate")
    target_link_options(WiiCompiled PRIVATE "LINKER:--wrap=rename")
    # Writes to the log (UserData/Logs/stderr.log) from every thread go one at a time
    # (runtime/horizon_unistd.c __wrap__write_r): libnx's fsdev write is not atomic per descriptor.
    target_link_options(WiiCompiled PRIVATE "LINKER:--wrap=_write_r")
    target_link_options(WiiCompiled PRIVATE "LINKER:-Map=${CMAKE_BINARY_DIR}/${FW_SWITCH_NRO_NAME}.map")
    nx_generate_nacp("${CMAKE_BINARY_DIR}/${FW_SWITCH_NRO_NAME}.nacp"
        NAME "FrightWorks" AUTHOR "FrightWorks contributors" VERSION "${FW_SWITCH_VERSION}")
    nx_create_nro(WiiCompiled
        OUTPUT "${CMAKE_BINARY_DIR}/${FW_SWITCH_NRO_NAME}.nro"
        NACP "${CMAKE_BINARY_DIR}/${FW_SWITCH_NRO_NAME}.nacp")
endfunction()
