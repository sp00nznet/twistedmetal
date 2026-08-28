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
cmake_minimum_required(VERSION 3.20)
project(twistedmetal CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 17)

set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ps3recomp" CACHE PATH "ps3recomp repo root")
set(PS3RECOMP_RUNTIME_LIB "${PS3RECOMP_DIR}/build/ps3recomp_runtime.lib"
    CACHE FILEPATH "prebuilt runtime library")

# Lifted game code (8 chunks, ~295 MB) + the generated HLE NID table.
file(GLOB RECOMP_SOURCES "src/recomp/*.cpp")
file(GLOB GEN_SOURCES    "src/gen/*.cpp")

add_executable(twistedmetal
    "${CMAKE_CURRENT_SOURCE_DIR}/src/boot_main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/hle_extra.cpp"     # imports the runtime lacks
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"        # vm + ELF loader + run
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"           # NID -> handler dispatch
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"        # boot-critical CRT
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"            # cellFs VFS
    ${GEN_SOURCES}
    ${RECOMP_SOURCES}
)

file(GLOB LIB_INC_DIRS LIST_DIRECTORIES true "${PS3RECOMP_DIR}/libs/*")

target_include_directories(twistedmetal PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/compat"            # <dirent.h>/<unistd.h> Win shims
    "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp"            # ppu_recomp.h
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/include"
    ${LIB_INC_DIRS}
)

target_link_libraries(twistedmetal PRIVATE "${PS3RECOMP_RUNTIME_LIB}")
if(WIN32)
    target_link_libraries(twistedmetal PRIVATE
        ws2_32 bcrypt xinput ole32 d3d12 dxgi dxguid d3dcompiler)
    # Every guest call is a host call, so recompiled call chains nest deeply.
    # Reserve 256 MB for the main thread; spawned threads get theirs in
    # sys_ppu_thread_create.
    target_link_options(twistedmetal PRIVATE /STACK:268435456 /MAP:twistedmetal.map)
endif()
