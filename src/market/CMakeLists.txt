# SQLite comes from the system: libsqlite3-dev on Debian, preinstalled on macOS.
# CMake ships FindSQLite3, so no external download or pinned hash is involved.
find_package(SQLite3 REQUIRED)

add_library(pricer_market
  stats.cpp
  rate_limiter.cpp
  cache.cpp
  finnhub.cpp
  service.cpp)

target_link_libraries(pricer_market
  PUBLIC pricer_core nlohmann_json::nlohmann_json
  PRIVATE SQLite::SQLite3)

target_include_directories(pricer_market PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)

target_compile_options(pricer_market PRIVATE ${PRICER_WARNINGS})

# HTTPS client for the provider. Without OpenSSL the build still succeeds but
# live fetches throw, so the service runs cache-only.
find_package(OpenSSL)
if(OpenSSL_FOUND)
  target_link_libraries(pricer_market PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  target_compile_definitions(pricer_market PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
  message(STATUS "pricer: OpenSSL found, live market data enabled")
else()
  message(WARNING "pricer: OpenSSL not found; the Finnhub provider will refuse to fetch")
endif()
target_link_libraries(pricer_market PRIVATE httplib::httplib)
