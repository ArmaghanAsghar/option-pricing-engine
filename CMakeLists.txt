cmake_minimum_required(VERSION 3.20)
project(pricer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Every executable lands in the build root regardless of which subdirectory
# defines it, so the Dockerfile and scripts have one predictable path.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

option(PRICER_BUILD_TESTS "Build the test suite" ON)
option(PRICER_BUILD_API   "Build the HTTP API server" ON)
option(PRICER_BUILD_UI    "Build the Qt desktop UI" OFF)
option(PRICER_BUILD_BENCH "Build the benchmark harness" ON)
option(PRICER_WERROR      "Treat warnings as errors" ON)

# The container builds with a newer GCC than its slim runtime base provides, so
# the C++ runtime is linked in statically. glibc, OpenSSL and SQLite stay
# dynamic: those are present in the base image and should keep receiving its
# security updates.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(PRICER_STATIC_CXX_DEFAULT ON)
else()
  set(PRICER_STATIC_CXX_DEFAULT OFF)
endif()
option(PRICER_STATIC_CXXLIB "Link libstdc++ and libgcc statically"
       ${PRICER_STATIC_CXX_DEFAULT})

function(pricer_link_runtime target)
  if(PRICER_STATIC_CXXLIB)
    target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc)
  endif()
endfunction()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------- dependencies

FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.15.3
  GIT_SHALLOW TRUE)
set(HTTPLIB_COMPILE OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(httplib)

# ------------------------------------------------------------------- core lib

add_library(pricer_core
  src/core/math.cpp
  src/core/types.cpp
  src/core/registry.cpp
  src/engines/black_scholes.cpp
  src/engines/binomial.cpp
  src/engines/monte_carlo.cpp
  src/engines/implied_vol.cpp)

target_include_directories(pricer_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

find_package(Threads REQUIRED)
target_link_libraries(pricer_core PUBLIC Threads::Threads)

set(PRICER_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
if(PRICER_WERROR)
  list(APPEND PRICER_WARNINGS -Werror)
endif()
target_compile_options(pricer_core PRIVATE ${PRICER_WARNINGS})

# JSON serialization shared by the CLI and the API.
add_library(pricer_json src/core/json_io.cpp)
target_link_libraries(pricer_json PUBLIC pricer_core nlohmann_json::nlohmann_json)
target_compile_options(pricer_json PRIVATE ${PRICER_WARNINGS})

# ------------------------------------------------------------------------ CLI

add_executable(pricer-cli src/cli/main.cpp)
target_link_libraries(pricer-cli PRIVATE pricer_json)
target_compile_options(pricer-cli PRIVATE ${PRICER_WARNINGS})
pricer_link_runtime(pricer-cli)

# ------------------------------------------------------------------------ API

if(PRICER_BUILD_API)
  add_subdirectory(src/market)
  add_subdirectory(src/api)
endif()

# ---------------------------------------------------------------------- tests

if(PRICER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(PRICER_BUILD_BENCH)
  add_executable(pricer-bench bench/main.cpp)
  target_link_libraries(pricer-bench PRIVATE pricer_core)
  target_compile_options(pricer-bench PRIVATE ${PRICER_WARNINGS})
  pricer_link_runtime(pricer-bench)
endif()

if(PRICER_BUILD_UI)
  add_subdirectory(ui)
endif()

install(TARGETS pricer-cli RUNTIME DESTINATION bin)
if(PRICER_BUILD_API)
  install(TARGETS pricer-api RUNTIME DESTINATION bin)
endif()
