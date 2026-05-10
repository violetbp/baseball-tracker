cmake_minimum_required(VERSION 3.20)
project(baseball-tracker-sim VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find packages
find_package(SDL2 REQUIRED)
find_package(SDL2_ttf REQUIRED)
find_package(OpenGL REQUIRED)

# Dear ImGui - use nixpkgs package
# Set these via environment or CMake config
if(DEFINED ENV{IMGUI_DIR})
    set(IMGUI_DIR $ENV{IMGUI_DIR})
else()
    # Fallback: clone ImGui if not provided
    set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../deps/imgui")
endif()

# ImGui sources (single-header + SDL2 backend + backends)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui/imgui.cpp
    ${IMGUI_DIR}/imgui/imgui_demo.cpp
    ${IMGUI_DIR}/imgui/imgui_draw.cpp
    ${IMGUI_DIR}/imgui/imgui_tables.cpp
    ${IMGUI_DIR}/imgui/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_backend.cpp
    ${IMGUI_DIR}/backends/imguiSDL2.cpp
)

# Main sources
set(SIM_SOURCES
    simulate.cpp
    esp_shim.cpp
    ../firmware/.esphome/external_components/b7d23dda/components/baseball_tracker/baseball_tracker.cpp
)

# Build executable
add_executable(baseball-tracker-sim ${SIM_SOURCES} ${IMGUI_SOURCES})

# Include directories
target_include_directories(baseball-tracker-sim PRIVATE
    ${SDL2_INCLUDE_DIR}
    ${SDL2_TTF_INCLUDE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ..
)

# Link libraries
target_link_libraries(baseball-tracker-sim PRIVATE
    ${SDL2_LIBRARY}
    ${SDL2_TTF_LIBRARY}
    ${OPENGL_LIBRARY}
)

# Platform libs
if(UNIX)
    target_link_libraries(baseball-tracker-sim PRIVATE
        dl
        pthread
    )
endif()

# Compiler flags
target_compile_options(baseball-tracker-sim PRIVATE
    $<$<CXX_COMPILER_ID:GNU>: -Wall -Wextra>
    $<$<CXX_COMPILER_ID:Clang>: -Wall -Wextra>
)

# Install
install(TARGETS baseball-tracker-sim RUNTIME DESTINATION bin)
