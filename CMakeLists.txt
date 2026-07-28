# Retcomm Launcher — multi-title hub for recomp/decomp projects.
#
#   cmake -G Ninja -S . -B build
#   cmake --build build -j
#   ./build/retcomm list

cmake_minimum_required(VERSION 3.24)
project(retcomm_launcher CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(retcomm
    src/main.cpp
    src/paths/paths.cpp
    src/core/hash.cpp
    src/core/config.cpp
    src/catalog/catalog.cpp
    src/romscan/romscan.cpp
    src/install/install.cpp
    src/romm/romm.cpp
)

target_include_directories(retcomm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# Bake the source-tree catalog path so `./build/retcomm` works without flags.
target_compile_definitions(retcomm PRIVATE
    "RETCOMM_CATALOG_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/catalog\""
)

# GCC < 9 needs -lstdc++fs for std::filesystem; newer libstdc++ folds it in.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(retcomm PRIVATE stdc++fs)
endif()

install(TARGETS retcomm RUNTIME DESTINATION bin)
install(DIRECTORY catalog/ DESTINATION share/retcomm/catalog)
