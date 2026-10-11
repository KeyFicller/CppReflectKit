cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# This repository is two things at once:
#
#   * the test project for the reflection layers, when it is the top-level
#     build -- it then owns the toolchain (clang-p2996), fetches every
#     dependency and registers the ctest targets;
#   * a set of importable, header-only libraries, when another project pulls
#     it in with FetchContent / add_subdirectory -- it then defines only the
#     library targets and leaves the consumer's compiler, dependencies and
#     test registration alone.
#
# The library targets, one per header:
#   cppreflect::undo   field capture / restore        (no external dependency)
#   cppreflect::ui     ImGui widgets                  (Dear ImGui)
#   cppreflect::json   JSON Schema + decoder          (nlohmann/json)
#   cppreflect::mcp    MCP server                     (cppreflect::json)
#   cppreflect::common the compile environment they all share
# ---------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(CPPREF_IS_TOP_LEVEL ON)
else()
  set(CPPREF_IS_TOP_LEVEL OFF)
endif()

# ---------------------------------------------------------------------------
# The clang-p2996 (P2996 static reflection) install. Resolved once, before
# project(), because a top-level build picks its compiler here. Precedence:
#
#   1. -DCPPREF_LLVM_ROOT=/path     explicit, always wins
#   2. $CPPREF_LLVM_ROOT            environment
#   3. the prefix of the C++ compiler already in use (<prefix>/bin/clang++)
#   4. a clang++ / clang++-reflect on PATH
#
# An install qualifies when it ships the reflective libc++ header, the one
# artifact a plain clang build does not have:
#   <CPPREF_LLVM_ROOT>/bin/clang++          compiler; the driver finds libc++ itself
#   <CPPREF_LLVM_ROOT>/include/c++/v1/meta  reflective libc++ headers
#   <CPPREF_LLVM_ROOT>/lib/libc++.dylib     matching runtime (rpath below)
# An importer keeps its own compiler; the library targets carry the rest.
# ---------------------------------------------------------------------------
if(NOT CPPREF_LLVM_ROOT AND DEFINED ENV{CPPREF_LLVM_ROOT})
  set(CPPREF_LLVM_ROOT "$ENV{CPPREF_LLVM_ROOT}")
endif()

if(NOT CPPREF_LLVM_ROOT AND CMAKE_CXX_COMPILER)
  get_filename_component(_cppref_prefix "${CMAKE_CXX_COMPILER}" DIRECTORY)
  get_filename_component(_cppref_prefix "${_cppref_prefix}" DIRECTORY)
  set(CPPREF_LLVM_ROOT "${_cppref_prefix}")
endif()

if(NOT CPPREF_LLVM_ROOT)
  find_program(CPPREF_CLANGXX NAMES clang++-reflect clang++)
  if(CPPREF_CLANGXX)
    get_filename_component(_cppref_prefix "${CPPREF_CLANGXX}" DIRECTORY)
    get_filename_component(_cppref_prefix "${_cppref_prefix}" DIRECTORY)
    set(CPPREF_LLVM_ROOT "${_cppref_prefix}")
  endif()
endif()

if(NOT CPPREF_LLVM_ROOT)
  message(FATAL_ERROR
    "clang-p2996 was not found. Build the fork's clang + libc++, then point at the "
    "install with -DCPPREF_LLVM_ROOT=/path, set the CPPREF_LLVM_ROOT environment "
    "variable, or put its clang++ on PATH.")
endif()

if(NOT EXISTS "${CPPREF_LLVM_ROOT}/include/c++/v1/meta")
  if(DEFINED CMAKE_CXX_COMPILER)
    set(_cppref_hint "\nCompiler in use: ${CMAKE_CXX_COMPILER}")
  else()
    set(_cppref_hint "")
  endif()
  message(FATAL_ERROR
    "No reflective libc++ under ${CPPREF_LLVM_ROOT}\n"
    "Expected ${CPPREF_LLVM_ROOT}/include/c++/v1/meta. CPPREF_LLVM_ROOT must name a "
    "clang-p2996 install built with libc++, since a plain clang install has no "
    "<meta>; set it explicitly when the compiler is reached through a symlink or a "
    "wrapper script.${_cppref_hint}")
endif()

set(CPPREF_LLVM_ROOT "${CPPREF_LLVM_ROOT}" CACHE PATH "clang-p2996 install prefix" FORCE)

if(CPPREF_IS_TOP_LEVEL)
  if(NOT EXISTS "${CPPREF_LLVM_ROOT}/bin/clang++")
    message(FATAL_ERROR "clang++ not found: ${CPPREF_LLVM_ROOT}/bin/clang++")
  endif()
  set(CMAKE_CXX_COMPILER "${CPPREF_LLVM_ROOT}/bin/clang++" CACHE FILEPATH "" FORCE)
  set(CMAKE_C_COMPILER "${CPPREF_LLVM_ROOT}/bin/clang" CACHE FILEPATH "" FORCE)
endif()

project(CppReflectKit LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# The environment a reflection build needs, discovered so that an importer sets
# nothing by hand:
#   CPPREF_LLVM_ROOT  resolved above, for the reflective libc++ a reflection
#                     binary links instead of /usr/lib
#   CPPREF_SDKROOT    the macOS SDK, since a source-built clang has no default
#                     sysroot
# Both ride on every library target below.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CPPREF_SDKROOT)
  execute_process(COMMAND xcrun --show-sdk-path
                  OUTPUT_VARIABLE _cppref_sdk OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE _cppref_sdk_rc ERROR_QUIET)
  if(_cppref_sdk_rc EQUAL 0 AND _cppref_sdk)
    set(CPPREF_SDKROOT "${_cppref_sdk}" CACHE PATH "macOS SDK sysroot for clang-p2996")
  endif()
endif()

# Top-level-only build hygiene.
if(CPPREF_IS_TOP_LEVEL)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  # Single-config generator (Ninja) would otherwise build with no flags at all,
  # i.e. no -g and therefore no debug info for breakpoints.
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
  endif()
  # A source-built clang has no default sysroot, and the fetched third-party
  # dependencies (glfw, googletest, ...) need one too; this applies to their
  # subdirectories, whereas the library targets carry their own copy below.
  if(CPPREF_SDKROOT)
    add_compile_options(-isysroot "${CPPREF_SDKROOT}")
    add_link_options(-isysroot "${CPPREF_SDKROOT}")
    add_link_options(-L "${CPPREF_LLVM_ROOT}/lib" "-Wl,-rpath,${CPPREF_LLVM_ROOT}/lib")
  endif()
endif()

# ---------------------------------------------------------------------------
# Options.
# ---------------------------------------------------------------------------
option(CPPREF_WITH_LLM "Define the json + mcp library targets" ON)
option(CPPREF_WITH_UI "Define the ui (ImGui widgets) library target" ON)
option(CPPREF_FETCH_DEPS "Fetch a dependency that find_package cannot provide" ON)
option(CPPREF_BUILD_TESTS "Build this repository's test targets" ${CPPREF_IS_TOP_LEVEL})
option(CPPREF_WARNINGS "Enable -Wall -Wextra on the test targets" ON)
option(CPPREF_SANITIZE "Build the test targets with ASan + UBSan" OFF)
set(CPPREF_IMGUI_ROOT "" CACHE PATH "An existing Dear ImGui checkout to use for ui")

# github.com clones are unreliable on this network, so fetch from the gitee
# mirrors by default (same tags/commits). Configure with -DCPPREF_DEPS_FROM_GITHUB=ON
# to use github.com directly.
option(CPPREF_DEPS_FROM_GITHUB "Fetch dependencies from github.com instead of the gitee mirrors" OFF)
if(CPPREF_DEPS_FROM_GITHUB)
  set(CPPREF_YAML_CPP_URL "https://github.com/jbeder/yaml-cpp.git" CACHE STRING "")
  set(CPPREF_GLFW_URL "https://github.com/glfw/glfw.git" CACHE STRING "")
  set(CPPREF_IMGUI_URL "https://github.com/ocornut/imgui.git" CACHE STRING "")
  set(CPPREF_NLOHMANN_JSON_URL "https://github.com/nlohmann/json.git" CACHE STRING "")
  set(CPPREF_GOOGLETEST_URL "https://github.com/google/googletest.git" CACHE STRING "")
else()
  set(CPPREF_YAML_CPP_URL "https://gitee.com/mirrors/yaml-cpp.git" CACHE STRING "")
  set(CPPREF_GLFW_URL "https://gitee.com/mirrors/glfw.git" CACHE STRING "")
  set(CPPREF_IMGUI_URL "https://gitee.com/mirrors/imgui.git" CACHE STRING "")
  set(CPPREF_NLOHMANN_JSON_URL "https://gitee.com/mirrors/nlohmann-json.git" CACHE STRING "")
  set(CPPREF_GOOGLETEST_URL "https://gitee.com/mirrors/googletest.git" CACHE STRING "")
endif()

include(FetchContent)

# yaml-cpp 0.8.0 declares a pre-3.5 cmake_minimum_required, which CMake 4 aborts on.
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

# ---------------------------------------------------------------------------
# The libraries. Each is an INTERFACE target -- the headers are the product --
# so a consumer links one name and gets the include root, the reflection flags
# and the dependency it needs.
# ---------------------------------------------------------------------------

# The compile environment every reflection translation unit needs.
add_library(cppreflect_common INTERFACE)
add_library(cppreflect::common ALIAS cppreflect_common)
target_include_directories(cppreflect_common INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/inc")
# <meta>, ^^ and [: :] are only reachable behind the fork's flag, so the target
# carries it. cxx_std_26 comes from CMake where known; otherwise set it here.
target_compile_options(cppreflect_common INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-freflection-latest>)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.25")
  target_compile_features(cppreflect_common INTERFACE cxx_std_26)
else()
  target_compile_options(cppreflect_common INTERFACE
      $<$<COMPILE_LANGUAGE:CXX>:-std=c++26>)
endif()
if(APPLE AND CPPREF_SDKROOT)
  target_compile_options(cppreflect_common INTERFACE -isysroot "${CPPREF_SDKROOT}")
  target_link_options(cppreflect_common INTERFACE -isysroot "${CPPREF_SDKROOT}")
endif()
if(EXISTS "${CPPREF_LLVM_ROOT}/lib/libc++.dylib")
  target_link_options(cppreflect_common INTERFACE
      -L "${CPPREF_LLVM_ROOT}/lib" "-Wl,-rpath,${CPPREF_LLVM_ROOT}/lib")
endif()

function(cppreflect_library _name)
  add_library(cppreflect_${_name} INTERFACE)
  add_library(cppreflect::${_name} ALIAS cppreflect_${_name})
  target_link_libraries(cppreflect_${_name} INTERFACE cppreflect_common)
endfunction()

cppreflect_library(undo)   # no external dependency

# --- json + mcp: need nlohmann/json ------------------------------------------
if(CPPREF_WITH_LLM OR CPPREF_BUILD_TESTS)
  # An importer that already provides the target (CampCat fetches nlohmann/json
  # itself) must not get a second, colliding definition of it.
  if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG QUIET)
    if(NOT nlohmann_json_FOUND)
      if(NOT CPPREF_FETCH_DEPS)
        message(FATAL_ERROR "nlohmann_json not found and CPPREF_FETCH_DEPS=OFF")
      endif()
      # nlohmann/json: validates the reflection-generated schemas.
      set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
      FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY "${CPPREF_NLOHMANN_JSON_URL}"
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
      )
      FetchContent_MakeAvailable(nlohmann_json)
    endif()
  endif()

  cppreflect_library(json)
  target_link_libraries(cppreflect_json INTERFACE nlohmann_json::nlohmann_json)

  cppreflect_library(mcp)
  target_link_libraries(cppreflect_mcp INTERFACE cppreflect_json)
endif()

# --- ui: needs Dear ImGui, which upstream ships without a CMakeLists ---------
if(CPPREF_WITH_UI OR CPPREF_BUILD_TESTS)
  set(_cppref_imgui_target "")

  # The ui/ test also needs ImGui's demo and backend sources, so a test build
  # always takes a full checkout; only an importer that just wants the widget
  # layer settles for a system package.
  if(CPPREF_IMGUI_ROOT)
    # an explicit checkout wins
  elseif(CPPREF_BUILD_TESTS)
    # fall through to the fetch below
  else()
    find_package(imgui CONFIG QUIET)
    if(TARGET imgui::imgui)
      set(_cppref_imgui_target imgui::imgui)
    endif()
  endif()

  if(NOT _cppref_imgui_target AND NOT CPPREF_IMGUI_ROOT)
    if(NOT CPPREF_FETCH_DEPS)
      message(FATAL_ERROR "Dear ImGui not found and CPPREF_FETCH_DEPS=OFF "
                          "(point CPPREF_IMGUI_ROOT at a checkout)")
    endif()
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
  endif()

  if(NOT _cppref_imgui_target)
    if(NOT EXISTS "${CPPREF_IMGUI_ROOT}/imgui.h")
      message(FATAL_ERROR "Dear ImGui sources not found at ${CPPREF_IMGUI_ROOT}")
    endif()
    # Core + the std::string helpers the widget layer includes; the GLFW/OpenGL
    # backends stay with whatever application opens a window.
    add_library(cppreflect_imgui STATIC
        "${CPPREF_IMGUI_ROOT}/imgui.cpp"
        "${CPPREF_IMGUI_ROOT}/imgui_draw.cpp"
        "${CPPREF_IMGUI_ROOT}/imgui_tables.cpp"
        "${CPPREF_IMGUI_ROOT}/imgui_widgets.cpp"
        "${CPPREF_IMGUI_ROOT}/misc/cpp/imgui_stdlib.cpp")
    target_include_directories(cppreflect_imgui PUBLIC
        "${CPPREF_IMGUI_ROOT}"
        "${CPPREF_IMGUI_ROOT}/backends"
        "${CPPREF_IMGUI_ROOT}/misc/cpp")
    set(_cppref_imgui_target cppreflect_imgui)
  endif()

  cppreflect_library(ui)
  target_link_libraries(cppreflect_ui INTERFACE ${_cppref_imgui_target})
endif()

# ---------------------------------------------------------------------------
# Tests. gtest_discover_tests registers one ctest entry per TEST(). ui/ opens a
# window and never returns, so it is registered but DISABLED -- run
# ./ui_reflection by hand. pytest covers the llm/ CLI + MCP contract the Python
# client depends on; see test/llm/test_contract.py.
# ---------------------------------------------------------------------------
if(CPPREF_BUILD_TESTS)

  # yaml-cpp 0.8.0 declares a pre-3.5 cmake_minimum_required, which CMake 4 aborts on.
  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY "${CPPREF_YAML_CPP_URL}"
    GIT_TAG 0.8.0
  )
  FetchContent_MakeAvailable(yaml-cpp)

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

  # GoogleTest: the assertion and test-runner framework for the test targets. With
  # it, compile-time expectations are static_assert and runtime ones are EXPECT_*;
  # there is no hand-rolled runner or macro.
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
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

  # --- basic/ : reflection language surface, reads yaml-cpp in one chapter ----
  add_executable(basic_reflection
    test/basic/test_hello.cpp
    test/basic/test_grammar.cpp
    test/basic/test_meta_functions.cpp
    test/basic/test_enumeration.cpp
    test/basic/test_struct.cpp
    test/basic/test_forward_chain.cpp)
  target_link_libraries(basic_reflection PRIVATE cppreflect::common yaml-cpp::yaml-cpp GTest::gtest_main)

  # --- undo/ : reflection-driven undo/redo --------------------------------
  add_executable(undo_reflection
    test/undo/test_undo_redo.cpp)
  target_link_libraries(undo_reflection PRIVATE cppreflect::undo GTest::gtest_main)

  # --- ui/ : reflection-driven widgets (interactive; not run by ctest) -----
  add_executable(ui_reflection
    test/ui/main.cpp
    test/ui/test_reflection_widgets.cpp
    test/ui/scaffold/imgui_backend_glfw_opengl3.cpp
    "${CPPREF_IMGUI_ROOT}/imgui_demo.cpp"
    "${CPPREF_IMGUI_ROOT}/backends/imgui_impl_glfw.cpp"
    "${CPPREF_IMGUI_ROOT}/backends/imgui_impl_opengl3.cpp")
  target_link_libraries(ui_reflection PRIVATE cppreflect::ui glfw OpenGL::GL)

  # --- llm/ : the app under test, plus its tests. -------------------------
  # scaffold/ holds the example application (model, server, transport, entries,
  # Python client); the two .cpp files beside it are the tests. The name stays
  # CppReflectKit because llm_client.py spawns build/CppReflectKit.
  add_executable(CppReflectKit
    test/llm/scaffold/main.cpp
    test/llm/scaffold/mcp_server.cpp
    test/llm/scaffold/bridge.cpp
    test/llm/test_json_schema.cpp
    test/llm/test_mcp.cpp)
  target_link_libraries(CppReflectKit PRIVATE cppreflect::mcp GTest::gtest)
  # The app's tools read the repository's own documentation; bake the source
  # directory in so the corpus is found whatever the working directory, and let
  # CPP_REFLECT_CORPUS override it at run time.
  target_compile_definitions(CppReflectKit PRIVATE
    CPPREF_CORPUS_ROOT="${CMAKE_SOURCE_DIR}")

  set(CPPREF_TEST_TARGETS basic_reflection undo_reflection ui_reflection CppReflectKit)

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

  enable_testing()
  include(GoogleTest)

  gtest_discover_tests(basic_reflection DISCOVERY_MODE PRE_TEST)
  gtest_discover_tests(undo_reflection DISCOVERY_MODE PRE_TEST)
  gtest_discover_tests(CppReflectKit DISCOVERY_MODE PRE_TEST)

  add_test(NAME ui COMMAND ui_reflection)
  set_tests_properties(ui PROPERTIES DISABLED TRUE LABELS manual)

endif()   # CPPREF_BUILD_TESTS
