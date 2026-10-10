cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# clang-p2996 (P2996 static reflection), installed locally:
#   <LLVM_ROOT>/bin/clang++          compiler; the driver finds the libc++ below
#   <LLVM_ROOT>/include/c++/v1/meta  reflective libc++ headers
#   <LLVM_ROOT>/lib/libc++.dylib     matching runtime
# ---------------------------------------------------------------------------
set(LLVM_ROOT "/Users/keyficller/src/llvm-p2996" CACHE PATH "clang-p2996 install prefix")

set(_cx "${LLVM_ROOT}/bin/clang++")
set(_cc "${LLVM_ROOT}/bin/clang")
if(NOT EXISTS "${_cx}")
  message(FATAL_ERROR "clang++ not found: ${_cx} (set LLVM_ROOT)")
endif()
set(CMAKE_CXX_COMPILER "${_cx}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER "${_cc}" CACHE FILEPATH "" FORCE)

project(CppReflection LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Single-config generator (Ninja) would otherwise build with no flags at all,
# i.e. no -g and therefore no debug info for breakpoints.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

if(NOT EXISTS "${LLVM_ROOT}/include/c++/v1/meta")
  message(FATAL_ERROR "reflective libc++ not found: ${LLVM_ROOT}/include/c++/v1/meta")
endif()

# macOS SDK headers: a source-built clang has no default sysroot.
execute_process(COMMAND xcrun --show-sdk-path
                OUTPUT_VARIABLE _sdk OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _sdk)
  message(FATAL_ERROR "xcrun --show-sdk-path failed")
endif()

add_compile_options(-isysroot "${_sdk}" "$<$<COMPILE_LANGUAGE:CXX>:-freflection-latest>")
# Link the reflective runtime from the prefix instead of /usr/lib.
add_link_options(-isysroot "${_sdk}" -L "${LLVM_ROOT}/lib" "-Wl,-rpath,${LLVM_ROOT}/lib")

# Diagnostics for this project's own target only; third-party code is left alone.
option(CPPREF_WARNINGS "Enable -Wall -Wextra on the test targets" ON)
option(CPPREF_SANITIZE "Build the test targets with ASan + UBSan" OFF)

# ---------------------------------------------------------------------------
# Third-party dependencies.
# ---------------------------------------------------------------------------
include(FetchContent)

# github.com clones are unreliable on this network, so fetch from the gitee
# mirrors by default (same tags/commits). Configure with -DCPPREF_DEPS_FROM_GITHUB=ON
# to use github.com directly.
option(CPPREF_DEPS_FROM_GITHUB "Fetch dependencies from github.com instead of the gitee mirrors" OFF)
if(CPPREF_DEPS_FROM_GITHUB)
  set(CPPREF_YAML_CPP_URL "https://github.com/jbeder/yaml-cpp.git" CACHE STRING "")
  set(CPPREF_GLFW_URL "https://github.com/glfw/glfw.git" CACHE STRING "")
  set(CPPREF_IMGUI_URL "https://github.com/ocornut/imgui.git" CACHE STRING "")
  set(CPPREF_NLOHMANN_JSON_URL "https://github.com/nlohmann/json.git" CACHE STRING "")
else()
  set(CPPREF_YAML_CPP_URL "https://gitee.com/mirrors/yaml-cpp.git" CACHE STRING "")
  set(CPPREF_GLFW_URL "https://gitee.com/mirrors/glfw.git" CACHE STRING "")
  set(CPPREF_IMGUI_URL "https://gitee.com/mirrors/imgui.git" CACHE STRING "")
  set(CPPREF_NLOHMANN_JSON_URL "https://gitee.com/mirrors/nlohmann-json.git" CACHE STRING "")
endif()

# yaml-cpp 0.8.0 declares a pre-3.5 cmake_minimum_required, which CMake 4 aborts on.
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY "${CPPREF_YAML_CPP_URL}"
  GIT_TAG 0.8.0
)
FetchContent_MakeAvailable(yaml-cpp)

# Dear ImGui (docking branch): sources only, the repo has no CMakeLists.
# Pinned to a commit, not the moving `docking` branch, so the build is
# reproducible; a pin also means no GIT_SHALLOW, which cannot fetch an
# arbitrary commit from every mirror.
set(CPPREF_IMGUI_TAG "7e1b65d26d52e9dd199d889c148c72184de647b4" CACHE STRING "Dear ImGui commit")
FetchContent_Declare(
  imgui
  GIT_REPOSITORY "${CPPREF_IMGUI_URL}"
  GIT_TAG "${CPPREF_IMGUI_TAG}"
)
FetchContent_MakeAvailable(imgui)
set(CPPREF_IMGUI_ROOT "${imgui_SOURCE_DIR}")
if(NOT EXISTS "${CPPREF_IMGUI_ROOT}/imgui.h")
  message(FATAL_ERROR "Dear ImGui sources not found at ${CPPREF_IMGUI_ROOT}")
endif()

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  glfw
  GIT_REPOSITORY "${CPPREF_GLFW_URL}"
  GIT_TAG 3.4
)
FetchContent_MakeAvailable(glfw)
find_package(OpenGL REQUIRED)

# nlohmann/json: used to validate the reflection-generated schemas.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY "${CPPREF_NLOHMANN_JSON_URL}"
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# GoogleTest: the assertion and test-runner framework for the test targets. With
# it, compile-time expectations are static_assert and runtime ones are EXPECT_*;
# there is no hand-rolled runner or macro.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
if(CPPREF_DEPS_FROM_GITHUB)
  set(CPPREF_GOOGLETEST_URL "https://github.com/google/googletest.git" CACHE STRING "")
else()
  set(CPPREF_GOOGLETEST_URL "https://gitee.com/mirrors/googletest.git" CACHE STRING "")
endif()
FetchContent_Declare(
  googletest
  GIT_REPOSITORY "${CPPREF_GOOGLETEST_URL}"
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

# gtest is third-party: leave its own sources alone rather than inheriting this
# project's warning set.
target_compile_options(gtest PRIVATE -Wno-character-conversion)

# ---------------------------------------------------------------------------
# One executable per test directory. basic/ and undo/ take their main() from
# gtest_main; llm/ keeps its own main because the binary doubles as the CLI/MCP
# process llm_client.py spawns. Each group links only what it uses.
# ---------------------------------------------------------------------------
set(CPPREF_TEST_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/inc")

set(IMGUI_SOURCES
    ${CPPREF_IMGUI_ROOT}/imgui.cpp
    ${CPPREF_IMGUI_ROOT}/imgui_demo.cpp
    ${CPPREF_IMGUI_ROOT}/imgui_draw.cpp
    ${CPPREF_IMGUI_ROOT}/imgui_tables.cpp
    ${CPPREF_IMGUI_ROOT}/imgui_widgets.cpp
    ${CPPREF_IMGUI_ROOT}/misc/cpp/imgui_stdlib.cpp
    ${CPPREF_IMGUI_ROOT}/backends/imgui_impl_glfw.cpp
    ${CPPREF_IMGUI_ROOT}/backends/imgui_impl_opengl3.cpp)

# --- basic/ : reflection language surface, reads yaml-cpp in one chapter ----
add_executable(basic_reflection
  test/basic/test_hello.cpp
  test/basic/test_grammar.cpp
  test/basic/test_meta_functions.cpp
  test/basic/test_enumeration.cpp
  test/basic/test_struct.cpp
  test/basic/test_forward_chain.cpp)
target_include_directories(basic_reflection PRIVATE ${CPPREF_TEST_INCLUDES})
target_link_libraries(basic_reflection PRIVATE yaml-cpp::yaml-cpp GTest::gtest_main)

# --- undo/ : reflection-driven undo/redo --------------------------------
add_executable(undo_reflection
  test/undo/test_undo_redo.cpp)
target_include_directories(undo_reflection PRIVATE ${CPPREF_TEST_INCLUDES})
target_link_libraries(undo_reflection PRIVATE GTest::gtest_main)

# --- ui/ : reflection-driven widgets (interactive; not run by ctest) -----
add_executable(ui_reflection
  test/ui/main.cpp
  test/ui/test_reflection_widgets.cpp
  test/ui/scaffold/imgui_backend_glfw_opengl3.cpp
  ${IMGUI_SOURCES})
target_include_directories(ui_reflection PRIVATE
    ${CPPREF_TEST_INCLUDES}
    "${CPPREF_IMGUI_ROOT}"
    "${CPPREF_IMGUI_ROOT}/backends"
    "${CPPREF_IMGUI_ROOT}/misc/cpp")
target_link_libraries(ui_reflection PRIVATE glfw OpenGL::GL)

# --- llm/ : the app under test, plus its tests. -------------------------
# scaffold/ holds the example application (model, server, transport, entries,
# Python client); the two .cpp files beside it are the tests. The name stays
# CppReflection because llm_client.py spawns build/CppReflection.
add_executable(CppReflection
  test/llm/scaffold/main.cpp
  test/llm/scaffold/mcp_server.cpp
  test/llm/scaffold/bridge.cpp
  test/llm/test_json_schema.cpp
  test/llm/test_mcp.cpp)
target_include_directories(CppReflection PRIVATE ${CPPREF_TEST_INCLUDES})
target_link_libraries(CppReflection PRIVATE nlohmann_json::nlohmann_json GTest::gtest)

set(CPPREF_TEST_TARGETS basic_reflection undo_reflection ui_reflection CppReflection)

# Diagnostics for this project's own targets only; third-party code is left alone.
foreach(_target IN LISTS CPPREF_TEST_TARGETS)
  if(CPPREF_WARNINGS)
    target_compile_options(${_target} PRIVATE -Wall -Wextra)
  endif()
  if(CPPREF_SANITIZE)
    target_compile_options(${_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${_target} PRIVATE -fsanitize=address,undefined)
  endif()
endforeach()

# ld64 leaves DWARF in the object files; lldb needs a .dSYM beside each binary
# to bind breakpoints. (macOS)
if(APPLE)
  find_program(CPPREF_DSYMUTIL dsymutil)
  if(CPPREF_DSYMUTIL)
    foreach(_target IN LISTS CPPREF_TEST_TARGETS)
      add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND "${CPPREF_DSYMUTIL}" "$<TARGET_FILE:${_target}>"
        COMMENT "Generating dSYM for ${_target}"
        VERBATIM)
    endforeach()
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests. gtest_discover_tests registers one ctest entry per TEST(). ui/ opens a
# window and never returns, so it is registered but DISABLED -- run
# ./ui_reflection by hand. pytest covers the llm/ CLI + MCP contract the Python
# client depends on; see test/llm/test_contract.py.
# ---------------------------------------------------------------------------
enable_testing()
include(GoogleTest)

gtest_discover_tests(basic_reflection DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(undo_reflection DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(CppReflection DISCOVERY_MODE PRE_TEST)

add_test(NAME ui COMMAND ui_reflection)
set_tests_properties(ui PROPERTIES DISABLED TRUE LABELS manual)
