include(FetchContent)

FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.2
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)

add_executable(pricer_tests
  test_math.cpp
  test_black_scholes.cpp
  test_binomial.cpp
  test_monte_carlo.cpp
  test_implied_vol.cpp
  test_json_io.cpp)

target_link_libraries(pricer_tests PRIVATE pricer_json Catch2::Catch2WithMain)
target_compile_options(pricer_tests PRIVATE -Wall -Wextra)

if(PRICER_BUILD_API)
  target_link_libraries(pricer_tests PRIVATE pricer_market)
  target_sources(pricer_tests PRIVATE test_market.cpp)
endif()

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(pricer_tests)
