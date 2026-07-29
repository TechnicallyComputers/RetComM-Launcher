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

# Embedded in binaries for Update RetComM / about text.
set(RETCOMM_VERSION "0.1.1" CACHE STRING "RetComM Launcher semantic version")
set(RETCOMM_GITHUB_SLUG "TechnicallyComputers/RetComM-Launcher" CACHE STRING
    "GitHub owner/repo used for self-update")

# Portable installs: look next to the binary first; baked path is a dev fallback.
option(RETCOMM_BUNDLE_CATALOG "Install catalog beside binaries (release layout)" ON)

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
    src/update/self_update.cpp
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
    "RETCOMM_VERSION=\"${RETCOMM_VERSION}\""
    "RETCOMM_GITHUB_SLUG=\"${RETCOMM_GITHUB_SLUG}\""
)
target_link_libraries(retcomm_core PUBLIC CURL::libcurl)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(retcomm_core PUBLIC stdc++fs)
endif()

add_executable(retcomm src/main.cpp)
target_link_libraries(retcomm PRIVATE retcomm_core)
if(UNIX AND NOT APPLE)
    set_target_properties(retcomm PROPERTIES INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN")
endif()

# ---------------------------------------------------------------------------
# Hub UI (SDL3 + OpenGL + Dear ImGui)
# ---------------------------------------------------------------------------
# Prefer a sibling recomp-ui checkout; otherwise FetchContent ocornut/imgui.
set(RETCOMM_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../recomp-ui/src/third_party/imgui"
    CACHE PATH "Path to Dear ImGui sources")

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

set(RETCOMM_HAVE_SDL3 FALSE)
set(RETCOMM_HUB_SDL_TARGET "")
if(TARGET SDL3::SDL3)
    set(RETCOMM_HAVE_SDL3 TRUE)
    set(RETCOMM_HUB_SDL_TARGET SDL3::SDL3)
elseif(TARGET PkgConfig::SDL3)
    set(RETCOMM_HAVE_SDL3 TRUE)
    set(RETCOMM_HUB_SDL_TARGET PkgConfig::SDL3)
endif()

set(RETCOMM_IMGUI_READY FALSE)
if(EXISTS "${RETCOMM_IMGUI_DIR}/imgui.cpp"
   AND EXISTS "${RETCOMM_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp")
    set(RETCOMM_IMGUI_READY TRUE)
elseif(RETCOMM_HAVE_SDL3)
    include(FetchContent)
    message(STATUS "retcomm-hub: fetching Dear ImGui (SDL3 backend)")
    FetchContent_Declare(retcomm_imgui_src
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.9-docking
        GIT_SHALLOW TRUE
    )
    # Populate only — do not add imgui's own CMake project.
    FetchContent_GetProperties(retcomm_imgui_src)
    if(NOT retcomm_imgui_src_POPULATED)
        FetchContent_Populate(retcomm_imgui_src)
    endif()
    set(RETCOMM_IMGUI_DIR "${retcomm_imgui_src_SOURCE_DIR}" CACHE PATH
        "Path to Dear ImGui sources" FORCE)
    if(EXISTS "${RETCOMM_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp")
        set(RETCOMM_IMGUI_READY TRUE)
    endif()
endif()

if(RETCOMM_HAVE_SDL3 AND RETCOMM_IMGUI_READY)
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
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    )
    # Optional: stb also available from a sibling recomp-ui tree.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../recomp-ui/src/third_party/stb_image.h")
        target_include_directories(retcomm-hub PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../recomp-ui/src/third_party)
    endif()
    target_link_libraries(retcomm-hub PRIVATE retcomm_core retcomm_imgui)
    if(UNIX AND NOT APPLE)
        set_target_properties(retcomm-hub PROPERTIES INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN")
    endif()
    install(TARGETS retcomm-hub RUNTIME DESTINATION bin)
    message(STATUS "retcomm-hub: enabled (ImGui=${RETCOMM_IMGUI_DIR})")
else()
    message(STATUS "retcomm-hub: skipped (need SDL3 + ImGui with imgui_impl_sdl3)")
endif()

install(TARGETS retcomm RUNTIME DESTINATION bin)
if(RETCOMM_BUNDLE_CATALOG)
    # Release layout: catalog/ next to binaries (also under share/ for FHS).
    install(DIRECTORY catalog/ DESTINATION catalog)
endif()
install(DIRECTORY catalog/ DESTINATION share/retcomm/catalog)
install(FILES assets/retcomm.svg DESTINATION share/icons/hicolor/scalable/apps
        RENAME retcomm.svg)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/retcomm.png")
    install(FILES assets/retcomm.png DESTINATION share/icons/hicolor/512x512/apps
            RENAME retcomm.png)
endif()

# Windows: ship vcpkg/shared runtime DLLs next to the installed exes.
if(WIN32)
    install(FILES $<TARGET_RUNTIME_DLLS:retcomm> DESTINATION bin COMPONENT Runtime)
    if(TARGET retcomm-hub)
        install(FILES $<TARGET_RUNTIME_DLLS:retcomm-hub> DESTINATION bin COMPONENT Runtime)
    endif()
endif()
