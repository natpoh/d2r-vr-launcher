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

# the built-in profiles of the two VR mods: the example manifests, turned into a header at configure time
set(VRML_BUILTIN_D2R_VR_FILE ${CMAKE_CURRENT_SOURCE_DIR}/examples/d2rloader-mods/d2r-vr/vrmod-manifest.json)
set(VRML_BUILTIN_D2R_PCVR_FILE ${CMAKE_CURRENT_SOURCE_DIR}/examples/d2rloader-mods/d2r-pcvr/vrmod-manifest.json)
file(READ ${VRML_BUILTIN_D2R_VR_FILE} VRML_BUILTIN_D2R_VR)
file(READ ${VRML_BUILTIN_D2R_PCVR_FILE} VRML_BUILTIN_D2R_PCVR)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${VRML_BUILTIN_D2R_VR_FILE} ${VRML_BUILTIN_D2R_PCVR_FILE})
configure_file(src/builtin_mods.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_mods.h @ONLY)

add_library(vrml STATIC
    src/json.cpp src/sha256.cpp src/util.cpp src/manifest.cpp src/registry.cpp src/game.cpp
    src/engine.cpp src/deps.cpp src/launch.cpp src/process.cpp src/mods.cpp src/cli.cpp
    src/net.cpp src/zip.cpp src/install.cpp src/releases.cpp src/setup_run.cpp src/cli_install.cpp src/install_run.cpp)
target_include_directories(vrml PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}/generated)
if(WIN32)
    target_link_libraries(vrml PUBLIC winhttp shell32 advapi32)   # HTTPS downloads (WinHTTP), running a Setup (ShellExecuteEx)
endif()
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

# What the window decides (games, mods, runtime, what Play may do), with no window in it: shared by the window, `--play` and the tests.
add_library(vrmlgui STATIC src/gui_model.cpp src/gui_install_model.cpp)
target_link_libraries(vrmlgui PUBLIC vrml)

# D2RVRLauncher.exe: the window (plain Win32 + GDI+, nothing else). Icons, pictures, manifest and version are in the exe (gui/launcher.rc).
if(WIN32)
    enable_language(RC)
    # the version of the exe is the launcher's: read from the one line in src/util.h
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/util.h)
    file(STRINGS src/util.h VRML_VERSION_LINE REGEX "kLauncherVersion = \"[0-9]+\\.[0-9]+\\.[0-9]+")
    string(REGEX MATCH "\"([0-9]+)\\.([0-9]+)\\.([0-9]+)([^\"]*)\"" VRML_VERSION_MATCH "${VRML_VERSION_LINE}")
    if(NOT VRML_VERSION_MATCH)
        message(FATAL_ERROR "kLauncherVersion in src/util.h must look like \"0.3.0\" (or \"0.3.0-draft\")")
    endif()
    set(VRML_VER_COMMA "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0")
    set(VRML_VER_TEXT "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}${CMAKE_MATCH_4}")
    configure_file(gui/gui_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/gui/gui_version.h @ONLY)

    add_executable(d2rvrlauncher WIN32 src/gui_main.cpp src/gui_window.cpp src/gui_install_view.cpp src/gui_draw.cpp gui/launcher.rc)
    set_target_properties(d2rvrlauncher PROPERTIES OUTPUT_NAME D2RVRLauncher)
    target_include_directories(d2rvrlauncher PRIVATE gui ${CMAKE_CURRENT_BINARY_DIR}/gui)
    target_compile_definitions(d2rvrlauncher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(d2rvrlauncher PRIVATE vrmlgui gdiplus comctl32 shell32 shlwapi uxtheme ole32 user32 gdi32 advapi32)
    if(MSVC)
        # launcher.manifest (in the .rc) is the only manifest; the linker's own would be a duplicate resource
        target_link_options(d2rvrlauncher PRIVATE /MANIFEST:NO)
    endif()
endif()

if(VRML_BUILD_TESTS)
    enable_testing()
    foreach(t json sha256 manifest engine deps launch cli examples mods zip net install releases setup_run cli_install install_run)
        add_executable(${t}_test tests/${t}_test.cpp)
        target_link_libraries(${t}_test PRIVATE vrml)
        target_compile_definitions(${t}_test PRIVATE VRML_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME ${t} COMMAND ${t}_test)
    endforeach()
    if(WIN32)
        # a stand-in D2RLoader.exe for the end-to-end tests (tools/e2e_mods.py)
        add_executable(fake_loader tests/tools/fake_loader.cpp)
        target_compile_options(fake_loader PRIVATE /utf-8 /W4 /EHsc /permissive-)
        target_compile_definitions(fake_loader PRIVATE UNICODE _UNICODE)
        # a stand-in for a mod's Setup (tools/install_smoke.py serves it as the "installer")
        add_executable(fake_setup tests/tools/fake_setup.cpp)
        target_compile_options(fake_setup PRIVATE /utf-8 /W4 /EHsc /permissive-)
        target_compile_definitions(fake_setup PRIVATE UNICODE _UNICODE)
    endif()
    add_executable(gui_model_test tests/gui_model_test.cpp)
    target_link_libraries(gui_model_test PRIVATE vrmlgui)
    target_compile_definitions(gui_model_test PRIVATE VRML_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME gui_model COMMAND gui_model_test)
    add_executable(gui_install_model_test tests/gui_install_model_test.cpp)
    target_link_libraries(gui_install_model_test PRIVATE vrmlgui)
    target_compile_definitions(gui_install_model_test PRIVATE VRML_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME gui_install_model COMMAND gui_install_model_test)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        # the JSON Schema file and the C++ validator must agree on the examples and the fixtures
        add_test(NAME schema_parity
                 COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_schema.py ${CMAKE_CURRENT_SOURCE_DIR}
                         $<TARGET_FILE:manifest_test>)
        if(WIN32)
            # the install commands against a local web server and a fake game folder (tools/install_smoke.py): no internet, no real game
            add_test(NAME install_smoke
                     COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/install_smoke.py --cli $<TARGET_FILE:vrmodlauncher>
                             --fake-setup $<TARGET_FILE:fake_setup> --work ${CMAKE_CURRENT_BINARY_DIR}/install_smoke_work)
            set_tests_properties(install_smoke PROPERTIES SKIP_RETURN_CODE 77)     # a game running on the machine makes every install refuse: skipped, not failed
            # the window, started for real against a fake game folder (tools/ui_smoke.py): shows a window for a moment
            add_test(NAME ui_smoke
                     COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/ui_smoke.py $<TARGET_FILE:d2rvrlauncher>
                             ${CMAKE_CURRENT_BINARY_DIR}/ui_smoke_work)
            # the install view and the update badges, for real, against a local web server (tools/install_view_smoke.py): no internet, no real game, nothing in the real launcher folder
            add_test(NAME install_view_smoke
                     COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/install_view_smoke.py --exe $<TARGET_FILE:d2rvrlauncher>
                             --fake-setup $<TARGET_FILE:fake_setup> --work ${CMAKE_CURRENT_BINARY_DIR}/install_view_work)
        endif()
    endif()
endif()
