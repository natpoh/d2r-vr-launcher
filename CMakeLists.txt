cmake_minimum_required(VERSION 3.20)
project(vr_launcher LANGUAGES CXX)

# The VR mod launcher: a draft of the shared launcher for D2R VR and d2r-pcvr (see SPEC.md).
# No dependencies, no network: C++17 and the standard library. Built on its own - not part of the mod's CMake tree - because it is
# meant to move to a repository of its own.
#
#   cmake -S vr_launcher -B build_vrl -G "Visual Studio 18 2026" -A x64        (any generator; Release or Debug)
#   cmake --build build_vrl --config Release
#   ctest --test-dir build_vrl -C Release --output-on-failure

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(VRML_BUILD_TESTS "Build the unit tests" ON)

add_library(vrml STATIC
    src/json.cpp src/sha256.cpp src/util.cpp src/manifest.cpp src/registry.cpp src/game.cpp
    src/engine.cpp src/deps.cpp src/launch.cpp src/process.cpp src/cli.cpp)
target_include_directories(vrml PUBLIC src)
if(MSVC)
    target_compile_options(vrml PUBLIC /utf-8 /W4 /EHsc /permissive-)
    target_compile_definitions(vrml PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(vrml PUBLIC -Wall -Wextra)
endif()

add_executable(vrmodlauncher src/main.cpp)
target_link_libraries(vrmodlauncher PRIVATE vrml)
if(WIN32)
    target_link_libraries(vrmodlauncher PRIVATE advapi32 shell32)
endif()

if(VRML_BUILD_TESTS)
    enable_testing()
    foreach(t json sha256 manifest engine deps launch cli examples)
        add_executable(${t}_test tests/${t}_test.cpp)
        target_link_libraries(${t}_test PRIVATE vrml)
        target_compile_definitions(${t}_test PRIVATE VRML_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME ${t} COMMAND ${t}_test)
    endforeach()
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        # the JSON Schema file and the C++ validator must agree on the examples and the fixtures
        add_test(NAME schema_parity
                 COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_schema.py ${CMAKE_CURRENT_SOURCE_DIR}
                         $<TARGET_FILE:manifest_test>)
    endif()
endif()
