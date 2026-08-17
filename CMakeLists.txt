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

# macOS: leave room in Mach-O load commands so packaging can rewrite Homebrew
# install names to @rpath / @loader_path without install_name_tool failing.
if(APPLE)
    add_link_options(-Wl,-headerpad_max_install_names)
endif()

# Embedded in binaries for Update RetComM / about text.
set(RETCOMM_VERSION "0.1.1" CACHE STRING "RetComM Launcher semantic version")
set(RETCOMM_GITHUB_SLUG "TechnicallyComputers/RetComM-Launcher" CACHE STRING
    "GitHub owner/repo used for self-update")
set(RETCOMM_CATALOG_GITHUB_REPO "TechnicallyComputers/retcomm-catalog" CACHE STRING
    "GitHub owner/repo for the title catalog")
set(RETCOMM_CATALOG_DOWNLOAD_URL
    "https://github.com/TechnicallyComputers/retcomm-catalog/releases/latest/download/catalog.zip"
    CACHE STRING "Default catalog.zip download URL")

# Portable installs: look next to the binary first; baked path is a dev fallback.
find_package(CURL REQUIRED)

set(RETCOMM_CORE_SOURCES
    src/paths/paths.cpp
    src/paths/process_env.cpp
    src/paths/toolchain_env.cpp
    src/core/hash.cpp
    src/core/config.cpp
    src/core/app_state.cpp
    src/core/texture_packs.cpp
    src/catalog/catalog.cpp
    src/catalog/catalog_sync.cpp
    src/romscan/romscan.cpp
    src/library/library_index.cpp
    src/bios/bios_index.cpp
    src/install/http.cpp
    src/install/install.cpp
    src/install/release_tags.cpp
    src/build/build.cpp
    src/build/cache_gc.cpp
    src/update/self_update.cpp
    src/launch/launch.cpp
    src/platform/psx_platform_settings.cpp
    src/platform/psx_input_profiles.cpp
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
    "RETCOMM_VERSION=\"${RETCOMM_VERSION}\""
    "RETCOMM_GITHUB_SLUG=\"${RETCOMM_GITHUB_SLUG}\""
    "RETCOMM_CATALOG_GITHUB_REPO=\"${RETCOMM_CATALOG_GITHUB_REPO}\""
    "RETCOMM_CATALOG_DOWNLOAD_URL=\"${RETCOMM_CATALOG_DOWNLOAD_URL}\""
)
target_link_libraries(retcomm_core PUBLIC CURL::libcurl)
if(WIN32)
    target_link_libraries(retcomm_core PUBLIC shell32)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(retcomm_core PUBLIC stdc++fs)
endif()

set(RETCOMM_WIN_RC "")
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/assets/retcomm.ico")
    string(REPLACE "\\" "/" RETCOMM_ICO_PATH "${CMAKE_SOURCE_DIR}/assets/retcomm.ico")
    configure_file("${CMAKE_SOURCE_DIR}/packaging/windows/retcomm.rc.in"
                   "${CMAKE_BINARY_DIR}/retcomm.rc" @ONLY)
    set(RETCOMM_WIN_RC "${CMAKE_BINARY_DIR}/retcomm.rc")
endif()

add_executable(retcomm src/main.cpp ${RETCOMM_WIN_RC})
target_link_libraries(retcomm PRIVATE retcomm_core)
if(UNIX AND NOT APPLE)
    set_target_properties(retcomm PROPERTIES INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN")
endif()

# Windows portable stub (payload appended at package time).
if(WIN32)
    add_executable(retcomm-portable WIN32
        src/portable/win_portable_main.cpp
        ${RETCOMM_WIN_RC}
    )
    target_compile_definitions(retcomm-portable PRIVATE
        "RETCOMM_VERSION=\"${RETCOMM_VERSION}\""
    )
    target_link_libraries(retcomm-portable PRIVATE shell32)
    install(TARGETS retcomm-portable RUNTIME DESTINATION bin)
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
    # FreeType enables CBDT/CBLC color emoji (Noto Color Emoji) via LoadColor.
    set(RETCOMM_IMGUI_FREETYPE FALSE)
    set(RETCOMM_IMGUI_FREETYPE_TARGET "")
    find_package(Freetype QUIET)
    if(TARGET Freetype::Freetype)
        set(RETCOMM_IMGUI_FREETYPE TRUE)
        set(RETCOMM_IMGUI_FREETYPE_TARGET Freetype::Freetype)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(RETCOMM_FT2 IMPORTED_TARGET freetype2)
            if(TARGET PkgConfig::RETCOMM_FT2)
                set(RETCOMM_IMGUI_FREETYPE TRUE)
                set(RETCOMM_IMGUI_FREETYPE_TARGET PkgConfig::RETCOMM_FT2)
            endif()
        endif()
    endif()

    set(RETCOMM_IMGUI_SOURCES
        ${RETCOMM_IMGUI_DIR}/imgui.cpp
        ${RETCOMM_IMGUI_DIR}/imgui_draw.cpp
        ${RETCOMM_IMGUI_DIR}/imgui_tables.cpp
        ${RETCOMM_IMGUI_DIR}/imgui_widgets.cpp
        ${RETCOMM_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
        ${RETCOMM_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
    # Layout matches upstream imgui: "misc/freetype/imgui_freetype.h" (see imgui_draw.cpp).
    set(RETCOMM_IMGUI_FREETYPE_SRC
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/misc/freetype/imgui_freetype.cpp")
    if(RETCOMM_IMGUI_FREETYPE AND EXISTS "${RETCOMM_IMGUI_FREETYPE_SRC}")
        list(APPEND RETCOMM_IMGUI_SOURCES ${RETCOMM_IMGUI_FREETYPE_SRC})
    else()
        set(RETCOMM_IMGUI_FREETYPE FALSE)
    endif()

    add_library(retcomm_imgui STATIC ${RETCOMM_IMGUI_SOURCES})
    target_include_directories(retcomm_imgui PUBLIC
        ${RETCOMM_IMGUI_DIR}
        ${RETCOMM_IMGUI_DIR}/backends
    )
    target_link_libraries(retcomm_imgui PUBLIC ${RETCOMM_HUB_SDL_TARGET} OpenGL::GL)
    if(RETCOMM_IMGUI_FREETYPE)
        # Prefer our vendored misc/ over a trimmed imgui tree that lacks it.
        # third_party → "misc/freetype/…"; misc/freetype → "imgui_freetype.h" in .cpp.
        target_include_directories(retcomm_imgui PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/misc/freetype)
        target_compile_definitions(retcomm_imgui PUBLIC IMGUI_ENABLE_FREETYPE)
        target_link_libraries(retcomm_imgui PUBLIC ${RETCOMM_IMGUI_FREETYPE_TARGET})
        message(STATUS "retcomm-hub: ImGui FreeType color-emoji support enabled")
    else()
        message(STATUS "retcomm-hub: ImGui FreeType not found — color emoji disabled")
    endif()

    # WIN32 → /SUBSYSTEM:WINDOWS so the hub has no console window on Windows.
    # CLI stays console (retcomm.exe). Portable stub is already WIN32.
    if(WIN32)
        add_executable(retcomm-hub WIN32
            src/hub/hub_main.cpp
            src/hub/hub_model.cpp
            src/hub/hub_boxart.cpp
            ${RETCOMM_WIN_RC}
        )
        # Hub uses int main(); MSVC Windows subsystem defaults to WinMain.
        if(MSVC)
            target_link_options(retcomm-hub PRIVATE "/ENTRY:mainCRTStartup")
        endif()
    else()
        add_executable(retcomm-hub
            src/hub/hub_main.cpp
            src/hub/hub_model.cpp
            src/hub/hub_boxart.cpp
        )
    endif()
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
install(FILES assets/retcomm.svg DESTINATION share/icons/hicolor/scalable/apps
        RENAME retcomm.svg)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/retcomm.png")
    install(FILES assets/retcomm.png DESTINATION share/icons/hicolor/512x512/apps
            RENAME retcomm.png)
    install(FILES assets/retcomm.png DESTINATION share/retcomm)
endif()
# Hub UI fonts (Lato Latin — same face as recomp-ui). Required when hub is built.
set(RETCOMM_FONT_REGULAR "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/LatoLatin-Regular.ttf")
if(TARGET retcomm-hub AND NOT EXISTS "${RETCOMM_FONT_REGULAR}")
    message(FATAL_ERROR
        "retcomm-hub needs ${RETCOMM_FONT_REGULAR} (and LatoLatin-Bold.ttf). "
        "Commit assets/fonts/ so AppImage/DMG/Windows packages ship the UI face.")
endif()
if(EXISTS "${RETCOMM_FONT_REGULAR}")
    install(FILES
        assets/fonts/LatoLatin-Regular.ttf
        assets/fonts/LatoLatin-Bold.ttf
        assets/fonts/NOTICE.md
        DESTINATION share/retcomm/fonts)
endif()
# Hub platform controller icons (library platform cards).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/platforms/psx.png")
    install(DIRECTORY assets/platforms/
            DESTINATION share/retcomm/platforms
            FILES_MATCHING PATTERN "*.png")
endif()
# PSX DualShock art for the Gamepads configure mapper.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/controllers/pad_analog.png")
    install(DIRECTORY assets/controllers/
            DESTINATION share/retcomm/controllers
            FILES_MATCHING PATTERN "*.png" PATTERN "*.tga")
endif()
# First-run setup path cards (Easy / Advanced).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/setup/setup_easy_rocket.png")
    install(DIRECTORY assets/setup/
            DESTINATION share/retcomm/setup
            FILES_MATCHING PATTERN "*.png")
endif()

# Windows: ship vcpkg/shared runtime DLLs next to the installed exes.
if(WIN32)
    install(FILES $<TARGET_RUNTIME_DLLS:retcomm> DESTINATION bin COMPONENT Runtime)
    if(TARGET retcomm-hub)
        install(FILES $<TARGET_RUNTIME_DLLS:retcomm-hub> DESTINATION bin COMPONENT Runtime)
    endif()
endif()
