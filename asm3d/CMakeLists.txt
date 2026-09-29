# ASM3D engine build.
#   Native (primary):   cmake -S . -B build -G Ninja && cmake --build build
#   Tests:              ./build/asm3d_tests
#   Wasm (secondary):   built automatically when clang + wasm-ld are available
#   Windows:            native with MinGW-w64 / MSYS2, or cross-compile from Linux:
#                       cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
cmake_minimum_required(VERSION 3.20)
project(ASM3D VERSION 0.1.0 LANGUAGES C)
if(WIN32)
  enable_language(RC)
endif()

option(ASM3D_USE_ASM "Use hand-written x86-64 assembly kernels" ON)
option(ASM3D_BUILD_WASM "Also build the WebAssembly runtime/tests (secondary target)" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(A3_USE_ASM_ACTIVE OFF)
if(ASM3D_USE_ASM AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  enable_language(ASM)
  set(A3_USE_ASM_ACTIVE ON)
endif()

set(A3_WARNINGS -Wall -Wextra -Wshadow -Wno-unused-parameter -Wno-missing-field-initializers)
# -ffp-contract=off: no FMA contraction, so native and wasm math are bit-identical.
set(A3_COMMON_FLAGS -ffp-contract=off -fno-strict-aliasing -fno-math-errno)

file(GLOB A3_CORE_SOURCES CONFIGURE_DEPENDS engine/core/*.c)
list(REMOVE_ITEM A3_CORE_SOURCES ${CMAKE_SOURCE_DIR}/engine/core/a3_simd_wasm.c)
# Platform-independent engine modules (compiled for native and wasm)
file(GLOB A3_SHARED_SOURCES CONFIGURE_DEPENDS
  engine/ecs/*.c
  engine/scene/*.c
  engine/input/*.c
  engine/physics/*.c
  engine/audio/*.c
  engine/particles/*.c
  engine/anim/*.c
  engine/script/*.c
  engine/modeling/*.c
  engine/resource/*.c
  engine/ui/*.c
)
# Renderer front-end is API independent; it talks to a backend through a3_rhi.h
list(APPEND A3_SHARED_SOURCES
  ${CMAKE_SOURCE_DIR}/engine/render/a3_mesh.c
  ${CMAKE_SOURCE_DIR}/engine/render/a3_renderer.c
  ${CMAKE_SOURCE_DIR}/engine/render/a3_shaders.c
  ${CMAKE_SOURCE_DIR}/engine/render/a3_shader_graph.c
)
set(A3_ENGINE_SOURCES
  ${A3_CORE_SOURCES}
  ${A3_SHARED_SOURCES}
  engine/platform/platform_posix.c
  engine/platform/platform_win32.c
  engine/jobs/a3_jobs.c
)
if(A3_USE_ASM_ACTIVE)
  list(APPEND A3_ENGINE_SOURCES engine/core/a3_simd_x64.S engine/physics/a3_physics_x64.S engine/audio/a3_audio_x64.S engine/particles/a3_particles_x64.S engine/modeling/a3_modeling_x64.S)
endif()

add_library(asm3d_engine STATIC ${A3_ENGINE_SOURCES})
target_include_directories(asm3d_engine PUBLIC engine)
target_compile_options(asm3d_engine PRIVATE $<$<COMPILE_LANGUAGE:C>:${A3_WARNINGS} ${A3_COMMON_FLAGS}>)
target_compile_options(asm3d_engine PUBLIC $<$<COMPILE_LANGUAGE:C>:${A3_COMMON_FLAGS}>)
if(A3_USE_ASM_ACTIVE)
  target_compile_definitions(asm3d_engine PUBLIC A3_USE_ASM=1)
endif()
if(WIN32)
  target_link_libraries(asm3d_engine PUBLIC winmm shell32 ole32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(asm3d_engine PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
if(MINGW)
  # self-contained .exe files: no MinGW runtime DLLs to ship
  add_link_options(-static -static-libgcc)
endif()

# ---- Desktop runtime (window, GPU, renderer, assets, engine loop) ----
if(WIN32 OR (UNIX AND NOT APPLE))
  file(GLOB A3_DESKTOP_SOURCES CONFIGURE_DEPENDS engine/runtime/*.c)
  list(APPEND A3_DESKTOP_SOURCES engine/render/rhi_gl.c engine/render/a3_gl.c)
  if(WIN32)
    list(APPEND A3_DESKTOP_SOURCES engine/platform/window_win32.c)
    add_library(asm3d_desktop STATIC ${A3_DESKTOP_SOURCES})
    target_link_libraries(asm3d_desktop PUBLIC asm3d_engine opengl32 gdi32 user32)
  else()
    find_package(X11 REQUIRED)
    list(APPEND A3_DESKTOP_SOURCES engine/platform/window_x11.c)
    add_library(asm3d_desktop STATIC ${A3_DESKTOP_SOURCES})
    target_link_libraries(asm3d_desktop PUBLIC asm3d_engine ${X11_LIBRARIES})
    target_include_directories(asm3d_desktop PUBLIC ${X11_INCLUDE_DIR})
  endif()
  target_compile_options(asm3d_desktop PRIVATE ${A3_WARNINGS})
  # Windows: GUI programs (no console window); they attach to the parent
  # console when started from a terminal so logs still show up.
  if(WIN32)
    set(A3_GUI WIN32)
    set(A3_ICON_RC ${CMAKE_SOURCE_DIR}/apps/asm3d.rc)
  endif()

  add_executable(asm3d_ui_gallery ${A3_GUI} apps/ui_gallery/main.c)
  target_link_libraries(asm3d_ui_gallery PRIVATE asm3d_desktop)

  add_executable(asm3d_player ${A3_GUI} apps/player/main.c ${A3_ICON_RC})
  target_link_libraries(asm3d_player PRIVATE asm3d_desktop)
  target_compile_options(asm3d_player PRIVATE ${A3_WARNINGS})

  # Command-line tool for automation and AI assistants (console program, JSON output)
  add_executable(asm3d_cli apps/cli/main.c)
  target_link_libraries(asm3d_cli PRIVATE asm3d_desktop)
  target_compile_options(asm3d_cli PRIVATE ${A3_WARNINGS})

  file(GLOB A3_EDITOR_SOURCES CONFIGURE_DEPENDS apps/editor/*.c)
  add_executable(asm3d_editor ${A3_GUI} ${A3_EDITOR_SOURCES} ${A3_ICON_RC})
  target_link_libraries(asm3d_editor PRIVATE asm3d_desktop)
  target_compile_options(asm3d_editor PRIVATE ${A3_WARNINGS})
endif()

# ---- Tests ----
file(GLOB A3_TEST_SOURCES CONFIGURE_DEPENDS tests/*.c)
add_executable(asm3d_tests ${A3_TEST_SOURCES} engine/render/rhi_null.c)
target_link_libraries(asm3d_tests PRIVATE asm3d_engine)
target_compile_options(asm3d_tests PRIVATE ${A3_WARNINGS})
target_compile_definitions(asm3d_tests PRIVATE A3_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
enable_testing()
add_test(NAME native_tests COMMAND asm3d_tests)

# ---- WebAssembly (secondary target) ----
# Freestanding build: no emscripten, no libc. The engine supplies its own
# allocator, formatter and math, so only clang + wasm-ld are needed.
if(ASM3D_BUILD_WASM AND NOT CMAKE_CROSSCOMPILING)
  find_program(A3_CLANG NAMES clang clang-18 clang-17 clang-16)
  find_program(A3_WASM_LD NAMES wasm-ld wasm-ld-18 wasm-ld-17)
  if(A3_CLANG AND A3_WASM_LD)
    set(A3_WASM_CFLAGS --target=wasm32 -ffreestanding -nostdlib -O2 -msimd128 -mbulk-memory
        -ffp-contract=off -fno-strict-aliasing -fno-math-errno -DNDEBUG=1 -Wall -Wno-unused-parameter
        -I${CMAKE_SOURCE_DIR}/engine)
    set(A3_WASM_LDFLAGS -fuse-ld=${A3_WASM_LD} -Wl,--no-entry -Wl,--export-dynamic
        -Wl,--allow-undefined -Wl,--gc-sections -Wl,-z,stack-size=1048576 -Wl,--initial-memory=4194304 -Wl,--strip-all)
    set(A3_WASM_ENGINE_SOURCES ${A3_CORE_SOURCES} ${CMAKE_SOURCE_DIR}/engine/core/a3_simd_wasm.c
        ${A3_SHARED_SOURCES} ${CMAKE_SOURCE_DIR}/engine/platform/platform_web.c ${CMAKE_SOURCE_DIR}/engine/jobs/a3_jobs.c
        ${CMAKE_SOURCE_DIR}/engine/render/rhi_null.c)
    set(A3_WASM_TEST_OUT ${CMAKE_BINARY_DIR}/asm3d_tests.wasm)
    add_custom_command(
      OUTPUT ${A3_WASM_TEST_OUT}
      COMMAND ${A3_CLANG} ${A3_WASM_CFLAGS} ${A3_WASM_LDFLAGS}
              ${A3_WASM_ENGINE_SOURCES} ${A3_TEST_SOURCES} -o ${A3_WASM_TEST_OUT}
      DEPENDS ${A3_WASM_ENGINE_SOURCES} ${A3_TEST_SOURCES}
      COMMENT "Building WebAssembly test module"
      VERBATIM)
    add_custom_target(asm3d_tests_wasm ALL DEPENDS ${A3_WASM_TEST_OUT})
    find_program(A3_NODE node)
    if(A3_NODE)
      add_test(NAME wasm_tests COMMAND ${A3_NODE} ${CMAKE_SOURCE_DIR}/tools/run_wasm_tests.mjs ${A3_WASM_TEST_OUT})
    endif()
  else()
    message(STATUS "ASM3D: clang/wasm-ld not found; skipping WebAssembly build")
  endif()
endif()
