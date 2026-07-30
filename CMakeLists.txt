cmake_minimum_required(VERSION 3.24)
project(infantry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# DirectXTK12: effects (built-in shaders), primitive batches, sprite/font
# rendering, texture loading, audio — the reusable layer above raw D3D12.
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_XAUDIO_WIN10 ON CACHE BOOL "" FORCE) # XAudio 2.9 backend for Sound.cpp
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

# nlohmann/json: parses the hand-authored level files in assets/levels/.
# Release tarball instead of the git repo — the repo's history is huge.
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(DirectXTK12 cgltf JoltPhysics json)

add_executable(infantry WIN32
    src/main.cpp
    src/Renderer.cpp
    src/Camera.cpp
    src/ClassSelect.cpp
    src/DebugText.cpp
    src/Game.cpp
    src/Level.cpp
    src/Model.cpp
    src/Physics.cpp
    src/Sound.cpp
    src/Visibility.cpp
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
    nlohmann_json::nlohmann_json
    d3d12
    dxgi
    user32
    xaudio2
)

# Ship models next to the exe so they can be loaded at runtime.
add_custom_command(TARGET infantry POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:infantry>/assets
)
