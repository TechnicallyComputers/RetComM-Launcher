# RetComM Launcher — multi-title hub for recomp/decomp projects.
#
#   cmake -G Ninja -S . -B build
#   cmake --build build -j
#   ./build/retcomm list
#   ./build/retcomm-hub

cmake_minimum_required(VERSION 3.24)
project(retcomm_launcher CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(CURL REQUIRED)

set(RETCOMM_CORE_SOURCES
    src/paths/paths.cpp
    src/core/hash.cpp
    src/core/config.cpp
    src/core/app_state.cpp
    src/catalog/catalog.cpp
    src/romscan/romscan.cpp
    src/library/library_index.cpp
    src/bios/bios_index.cpp
    src/install/http.cpp
    src/install/install.cpp
    src/launch/launch.cpp
    src/romm/romm.cpp
    src/romm/romm_fetch.cpp
    src/romm/romm_saves.cpp
)

add_library(retcomm_core STATIC ${RETCOMM_CORE_SOURCES})
target_include_directories(retcomm_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(retcomm_core PUBLIC
    "RETCOMM_CATALOG_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/catalog\""
)
target_link_libraries(retcomm_core PUBLIC CURL::libcurl)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(retcomm_core PUBLIC stdc++fs)
endif()

add_executable(retcomm src/main.cpp)
target_link_libraries(retcomm PRIVATE retcomm_core)

# ---------------------------------------------------------------------------
# Hub UI (SDL3 + OpenGL + Dear ImGui from sibling recomp-ui tree)
# ---------------------------------------------------------------------------
set(RETCOMM_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../recomp-ui/src/third_party/imgui"
    CACHE PATH "Path to Dear ImGui sources (recomp-ui third_party/imgui)")

find_package(SDL3 QUIET)
find_package(OpenGL QUIET)
if(NOT TARGET OpenGL::GL)
    find_package(OpenGL REQUIRED)
endif()

if(NOT SDL3_FOUND AND NOT TARGET SDL3::SDL3)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL3 IMPORTED_TARGET sdl3)
    endif()
endif()

if(EXISTS "${RETCOMM_IMGUI_DIR}/imgui.cpp" AND (TARGET SDL3::SDL3 OR TARGET PkgConfig::SDL3))
    set(RETCOMM_HUB_SDL_TARGET SDL3::SDL3)
    if(TARGET PkgConfig::SDL3 AND NOT TARGET SDL3::SDL3)
        set(RETCOMM_HUB_SDL_TARGET PkgConfig::SDL3)
    endif()

    add_library(retcomm_imgui STATIC
        ${RETCOMM_IMGUI_DIR}/imgui.cpp
        ${RETCOMM_IMGUI_DIR}/imgui_draw.cpp
        ${RETCOMM_IMGUI_DIR}/imgui_tables.cpp
        ${RETCOMM_IMGUI_DIR}/imgui_widgets.cpp
        ${RETCOMM_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
        ${RETCOMM_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(retcomm_imgui PUBLIC
        ${RETCOMM_IMGUI_DIR}
        ${RETCOMM_IMGUI_DIR}/backends
    )
    target_link_libraries(retcomm_imgui PUBLIC ${RETCOMM_HUB_SDL_TARGET} OpenGL::GL)

    add_executable(retcomm-hub
        src/hub/hub_main.cpp
        src/hub/hub_model.cpp
        src/hub/hub_boxart.cpp
    )
    target_include_directories(retcomm-hub PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../recomp-ui/src/third_party
    )
    target_link_libraries(retcomm-hub PRIVATE retcomm_core retcomm_imgui)
    install(TARGETS retcomm-hub RUNTIME DESTINATION bin)
    message(STATUS "retcomm-hub: enabled (ImGui=${RETCOMM_IMGUI_DIR})")
else()
    message(STATUS "retcomm-hub: skipped (need SDL3 + ImGui at ${RETCOMM_IMGUI_DIR})")
endif()

install(TARGETS retcomm RUNTIME DESTINATION bin)
install(DIRECTORY catalog/ DESTINATION share/retcomm/catalog)
