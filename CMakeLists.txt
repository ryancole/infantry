cmake_minimum_required(VERSION 3.24)
project(infantry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(infantry WIN32
    src/main.cpp
    src/Renderer.cpp
    src/Camera.cpp
    src/Game.cpp
)

target_compile_definitions(infantry PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(infantry PRIVATE
    d3d12
    dxgi
    d3dcompiler
    user32
)

# Ship shaders next to the exe so they can be loaded at runtime.
add_custom_command(TARGET infantry POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/src/shaders
        $<TARGET_FILE_DIR:infantry>/shaders
)
