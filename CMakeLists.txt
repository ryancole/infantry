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

# toml++: reads and writes the player's key bindings. TOML rather than the JSON
# already here because this is the one file a person is expected to open — it
# takes comments, which is where the file explains itself.
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
)

# ENet: reliable-where-asked UDP for the client/server link. Chosen over raw
# sockets for exactly two services — connection lifecycle and optional
# per-channel reliability — and over heavier stacks because those two are all
# the netcode wants from a library; everything above them (snapshots, commands,
# events) is ours in Net.h.
FetchContent_Declare(enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG v1.3.18
)

FetchContent_MakeAvailable(DirectXTK12 cgltf JoltPhysics json tomlplusplus enet)

# The simulation and the host around it, compiled once and linked into both
# executables below. What earns a file a place here is that a headless server
# needs it: the World, everything the World calls, and the Server that runs a
# match over a socket. Nothing in this list may touch the renderer, the sound
# device, or a window — the server target existing is what enforces that, so a
# rendering include creeping in fails a build instead of a code review.
#
# The client links all of it too, and not as a leftover: playing on your own
# starts one of these Servers in-process and joins it over loopback, so there
# is one implementation of "a match" and both binaries are running it.
set(SIM_SOURCES
    src/World.cpp
    src/Ability.cpp
    src/Brain.cpp
    src/Discovery.cpp
    src/Level.cpp
    src/Physics.cpp
    src/Server.cpp
    src/Visibility.cpp
)

add_executable(infantry WIN32
    src/main.cpp
    src/Renderer.cpp
    src/AbilityDraw.cpp
    src/AudioMenu.cpp
    src/BindMenu.cpp
    src/Bindings.cpp
    src/Camera.cpp
    src/ClassSelect.cpp
    src/Game.cpp
    src/Hud.cpp
    src/JoinMenu.cpp
    src/MainMenu.cpp
    src/Model.cpp
    src/NetClient.cpp
    src/OptionsMenu.cpp
    src/PlayerMenu.cpp
    src/Settings.cpp
    src/Soldier.cpp
    src/Sound.cpp
    ${SIM_SOURCES}
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
    tomlplusplus::tomlplusplus
    enet
    d3d12
    dxgi
    user32
    shell32
    xaudio2
    ws2_32
    winmm
)
target_include_directories(infantry PRIVATE ${enet_SOURCE_DIR}/include)

# Ship models next to the exe so they can be loaded at runtime.
add_custom_command(TARGET infantry POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:infantry>/assets
)

# The dedicated server: the same Server the client embeds, with a console loop
# around it and no renderer, no audio, no window. Note what's absent from its
# link line — d3d12, dxgi, user32, xaudio2 — which is the point of the target:
# if the simulation ever grows a quiet dependency on any of them, this is the
# build that breaks. DirectXTK12 stays because SimpleMath lives there; nothing
# device-shaped is pulled from it.
add_executable(infantry_server
    src/server_main.cpp
    ${SIM_SOURCES}
)

target_compile_definitions(infantry_server PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(infantry_server PRIVATE
    DirectXTK12
    Jolt
    nlohmann_json::nlohmann_json
    enet
    ws2_32
    winmm
)
target_include_directories(infantry_server PRIVATE ${enet_SOURCE_DIR}/include)

# The server reads the same level files the client does.
add_custom_command(TARGET infantry_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:infantry_server>/assets
)
