# Twisted Metal (BCUS98106) — PPU boot harness.
#
# Wires the lifter output to ps3recomp's generic first-boot harness: load the
# EBOOT image -> register the lifted functions + HLE NID handlers -> dispatch
# the entry OPD -> run real game code until it hits an unlifted function, an
# unimplemented import or an lv2 syscall, each logged so the next thing to
# implement is obvious.
#
# Toolchain: clang-cl (the loader uses __builtin_bswap and weak symbols MSVC
# lacks; clang-cl keeps the MSVC ABI so it links the prebuilt runtime .lib).
#   cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
#   cmake --build build
#   ./build/twistedmetal input/EBOOT.ELF
#
# Linux (GCC or Clang): the runtime is built here from PS3RECOMP_DIR, together
# with its dependencies (SDL2, Threads, glslang when Vulkan is on).
#   cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-linux
#
# Without any game data: -DTM_BUILD_GAME=OFF -DTM_BUILD_SMOKE=ON builds
# twistedmetal_smoke, this repo's harness booting ps3recomp's smoke title.
#   ./build-linux/twistedmetal_smoke build-linux/smoke/smoke.elf   -> [smoke] PASS
cmake_minimum_required(VERSION 3.20)
project(twistedmetal CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 17)

set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ps3recomp" CACHE PATH "ps3recomp repo root")
if(WIN32)
    set(PS3RECOMP_RUNTIME_LIB "${PS3RECOMP_DIR}/build/ps3recomp_runtime.lib"
        CACHE FILEPATH "prebuilt runtime library")
else()
    add_subdirectory("${PS3RECOMP_DIR}" ps3recomp EXCLUDE_FROM_ALL)
endif()

option(TM_BUILD_GAME  "Build the game executable (needs the lift in TM_RECOMP_DIR)" ON)
option(TM_BUILD_SMOKE "Build twistedmetal_smoke: this harness on ps3recomp's smoke title" OFF)
set(TM_RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp" CACHE PATH
    "lifted ppu_recomp.h and its chunks")
# Extra compile options for the lifted chunks only (e.g. -O1 to keep GCC's
# memory down on 40 MB translation units). Empty: the build type's own.
set(TM_RECOMP_COMPILE_OPTIONS "" CACHE STRING "extra options for the lifted chunks")

# PPU boot scaffold, compiled per game against the lifter's ppu_recomp.h.
set(TM_SCAFFOLD_SOURCES
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"        # vm + ELF loader + run
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"           # NID -> handler dispatch
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"        # boot-critical CRT
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"            # cellFs VFS
)

file(GLOB LIB_INC_DIRS LIST_DIRECTORIES true "${PS3RECOMP_DIR}/libs/*")

# Include paths, runtime and platform libraries shared by the game and the smoke
# target. recomp_dir is where that target's ppu_recomp.h lives.
function(tm_configure_target tgt recomp_dir)
    if(WIN32)
        target_include_directories(${tgt} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/compat"    # <dirent.h>/<unistd.h> Win shims
            "${recomp_dir}"                             # ppu_recomp.h
            "${PS3RECOMP_DIR}/runtime/ppu"
            "${PS3RECOMP_DIR}/runtime/spu"
            "${PS3RECOMP_DIR}/include"
            ${LIB_INC_DIRS}
        )
        target_link_libraries(${tgt} PRIVATE "${PS3RECOMP_RUNTIME_LIB}")
        target_link_libraries(${tgt} PRIVATE
            ws2_32 bcrypt xinput ole32 d3d12 dxgi dxguid d3dcompiler
            mfplat mfuuid)   # Media Foundation H.264 decoder (system component)
        # Every guest call is a host call, so recompiled call chains nest deeply.
        # Reserve 256 MB for the main thread; spawned threads get theirs in
        # sys_ppu_thread_create.
        target_link_options(${tgt} PRIVATE /STACK:268435456 /MAP:${tgt}.map)
    else()
        # No Win32 shims here: they would shadow <dirent.h>/<unistd.h>. The main
        # guest thread's 256 MB stack is set up in boot_main.cpp instead.
        target_include_directories(${tgt} PRIVATE
            "${recomp_dir}"
            "${PS3RECOMP_DIR}/runtime/ppu"
            "${PS3RECOMP_DIR}/runtime/spu"
            "${PS3RECOMP_DIR}/runtime/memory"
            "${PS3RECOMP_DIR}/runtime/syscalls"
            "${PS3RECOMP_DIR}/runtime/platform"
            "${PS3RECOMP_DIR}/include"
            ${LIB_INC_DIRS}
        )
        find_package(Threads REQUIRED)
        target_link_libraries(${tgt} PRIVATE ps3recomp_runtime Threads::Threads ${CMAKE_DL_LIBS})
        target_compile_options(${tgt} PRIVATE -fno-strict-aliasing)
        set_target_properties(${tgt} PROPERTIES LINKER_LANGUAGE CXX)
    endif()
endfunction()

if(TM_BUILD_GAME)
    # Lifted game code (12 chunks, ~460 MB) + the generated HLE NID table.
    file(GLOB RECOMP_SOURCES "${TM_RECOMP_DIR}/*.cpp")
    file(GLOB GEN_SOURCES    "src/gen/*.cpp" "src/gen/*.c")
    # Lifted SPU images, one directory per image, plus the workload registry that
    # maps each guest image's fingerprint to its lifted entry point.
    file(GLOB_RECURSE SPU_SOURCES "src/spu_gen/*.c")
    # spu_jobs.c registers the SPU jobs lifted under src/spu_gen and cannot link
    # without them. A first boot does not need them.
    if(NOT SPU_SOURCES)
        list(FILTER GEN_SOURCES EXCLUDE REGEX "/spu_jobs\\.c$")
        message(STATUS "twistedmetal: no lifted SPU code in src/spu_gen; spu_jobs.c left out")
    endif()

    add_executable(twistedmetal
        "${CMAKE_CURRENT_SOURCE_DIR}/src/boot_main.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/hle_extra.cpp"     # imports the runtime lacks
        "${CMAKE_CURRENT_SOURCE_DIR}/src/tm_inflate.cpp"    # host DEFLATE for the SPU decompressor
        "${CMAKE_CURRENT_SOURCE_DIR}/src/vdec_mf.cpp"       # H.264 for cellVdec, on the Media Foundation MFT
        "${CMAKE_CURRENT_SOURCE_DIR}/src/vdec_hle.cpp"      # the cellVdec*Ex entry points the title actually calls
        ${TM_SCAFFOLD_SOURCES}
        ${GEN_SOURCES}
        ${SPU_SOURCES}
        ${RECOMP_SOURCES}
    )
    tm_configure_target(twistedmetal "${TM_RECOMP_DIR}")
    if(NOT MSVC)
        # Generated code: warnings bury what matters, and a PPC fmadd must not be
        # fused or split differently from what the lifter wrote.
        set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
            COMPILE_OPTIONS "-w;-ffp-contract=off;${TM_RECOMP_COMPILE_OPTIONS}")
    endif()
endif()

if(TM_BUILD_SMOKE)
    # ps3recomp's smoke title: a hand-written program in the lifter's ABI that
    # checks the loader, the NID bridge, lv2, a guest thread, the frame clock,
    # the RSX backend and sys_process_exit. Generated here rather than taken
    # from ps3recomp's own build, which only makes it on UNIX.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(TM_SMOKE_GEN "${CMAKE_CURRENT_BINARY_DIR}/smoke")
    add_custom_command(
        OUTPUT "${TM_SMOKE_GEN}/smoke.elf" "${TM_SMOKE_GEN}/ppu_recomp.h"
        COMMAND "${Python3_EXECUTABLE}" "${PS3RECOMP_DIR}/tools/make_smoke_elf.py"
                --out "${TM_SMOKE_GEN}/smoke.elf" --header "${TM_SMOKE_GEN}/ppu_recomp.h"
        DEPENDS "${PS3RECOMP_DIR}/tools/make_smoke_elf.py" "${PS3RECOMP_DIR}/tools/ppu_lifter.py"
        COMMENT "Generating the smoke title's image and ppu_recomp.h")
    add_custom_command(
        OUTPUT "${TM_SMOKE_GEN}/ppu_hle_nids.cpp"
        COMMAND "${Python3_EXECUTABLE}" "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${TM_SMOKE_GEN}/ppu_hle_nids.cpp" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py" "${PS3RECOMP_DIR}/tools/nid_database.py"
        COMMENT "Generating the HLE NID -> handler table")

    add_executable(twistedmetal_smoke
        "${CMAKE_CURRENT_SOURCE_DIR}/src/boot_main.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/smoke_stubs.cpp"   # the tm_* hooks, empty
        ${TM_SCAFFOLD_SOURCES}
        "${PS3RECOMP_DIR}/runtime/ppu/tests/smoke/smoke_recomp.c"
        "${TM_SMOKE_GEN}/ppu_hle_nids.cpp"
        "${TM_SMOKE_GEN}/ppu_recomp.h"
    )
    tm_configure_target(twistedmetal_smoke "${TM_SMOKE_GEN}")
endif()
