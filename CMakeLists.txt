cmake_minimum_required(VERSION 3.24)
project(infantry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# DirectXTK12: effects (built-in shaders), primitive batches, sprite/font
# rendering, texture loading, audio — the reusable layer above raw D3D12.
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_XAUDIO_WIN10 OFF CACHE BOOL "" FORCE) # flip on when audio lands
FetchContent_Declare(DirectXTK12
    GIT_REPOSITORY https://github.com/microsoft/DirectXTK12.git
    GIT_TAG may2026
)

# cgltf: single-header glTF 2.0 parser used by the model loader.
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG v1.15
)

# Jolt: rigid-body physics (projectiles, world collision). Deterministic on a
# given binary, which keeps lockstep net play on the table.
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE) # match our /MD CRT
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.6.0
    SOURCE_SUBDIR Build
)

FetchContent_MakeAvailable(DirectXTK12 cgltf JoltPhysics)

add_executable(infantry WIN32
    src/main.cpp
    src/Renderer.cpp
    src/Camera.cpp
    src/Game.cpp
    src/Model.cpp
    src/Physics.cpp
)

target_compile_definitions(infantry PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(infantry PRIVATE ${cgltf_SOURCE_DIR})

target_link_libraries(infantry PRIVATE
    DirectXTK12
    Jolt
    d3d12
    dxgi
    user32
)

# Ship models next to the exe so they can be loaded at runtime.
add_custom_command(TARGET infantry POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:infantry>/assets
)
