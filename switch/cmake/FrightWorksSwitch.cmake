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
# deko3d. FW_SWITCH_DEKO turns on the deko3d interposer for H3 and is not wired yet.
#
# FW_SWITCH_AUDIT=ON with RECOMP_BUILD_PRODUCTS=OFF skips Dawn, the SDL shim and Aurora and compiles
# just the runtime's own sources for the Switch (recomp_switch_audit): no network, no translated game.

cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH FW_SWITCH_DIR)
set(RECOMP_RUNTIME_DIR_FOR_SWITCH "${CMAKE_CURRENT_SOURCE_DIR}")

option(FW_SWITCH_AUDIT "Switch: compile only the runtime's sources (no Dawn, SDL shim or Aurora)" OFF)
option(FW_SWITCH_DEKO "Switch: deko3d renderer through the wgpu interposer (H3, not wired yet)" OFF)
option(FW_SWITCH_DEBUG_SERVER "Switch: start the debug server (TCP 6543, no password) at boot" ON)
option(FW_SWITCH_CHECKED_GUEST_MEMORY "Switch: check every guest memory access (no flat window)" OFF)
# The NRO, its ELF and its SD folder (sdmc:/switch/no100f-nx/), named after the clone like spm-nx.
set(FW_SWITCH_NRO_NAME "no100f-nx" CACHE STRING "Switch: file name of the NRO, its ELF and its SD folder")
set(FW_SWITCH_SDL3_HEADERS "" CACHE PATH
    "Switch: an SDL 3.4.4 source tree for its headers (empty: download the pinned release)")
set(FW_SWITCH_IMGUI_DIR "" CACHE PATH
    "Switch audit only: an ImGui v1.91.9b-docking tree for its headers (empty: download it)")
set(FW_SWITCH_VERSION "0.0.1" CACHE STRING "Switch: NACP version")

if(FW_SWITCH_DEKO)
    message(FATAL_ERROR "FW_SWITCH_DEKO is the H3 milestone and is not wired yet; H2 runs on Dawn Null")
endif()

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
    set_target_properties(WiiCompiled PROPERTIES OUTPUT_NAME "${FW_SWITCH_NRO_NAME}" SUFFIX ".elf")
    target_link_libraries(WiiCompiled PRIVATE SDL3::SDL3-static nx)
    # Every pthread and std::thread gets a 4 MiB stack and the application cores, and detaching one
    # works (runtime/horizon_pthread.c): libnx's default 128 KiB overflows in Tint, and libnx has no
    # working pthread_detach. rename() replaces an existing file the way POSIX does (horizon_unistd.c).
    target_link_options(WiiCompiled PRIVATE "LINKER:--wrap=pthread_create" "LINKER:--wrap=pthread_detach")
    target_link_options(WiiCompiled PRIVATE "LINKER:--wrap=threadCreate")
    target_link_options(WiiCompiled PRIVATE "LINKER:--wrap=rename")
    target_link_options(WiiCompiled PRIVATE "LINKER:-Map=${CMAKE_BINARY_DIR}/${FW_SWITCH_NRO_NAME}.map")
    nx_generate_nacp("${CMAKE_BINARY_DIR}/${FW_SWITCH_NRO_NAME}.nacp"
        NAME "FrightWorks" AUTHOR "FrightWorks contributors" VERSION "${FW_SWITCH_VERSION}")
    nx_create_nro(WiiCompiled
        OUTPUT "${CMAKE_BINARY_DIR}/${FW_SWITCH_NRO_NAME}.nro"
        NACP "${CMAKE_BINARY_DIR}/${FW_SWITCH_NRO_NAME}.nacp")
endfunction()
