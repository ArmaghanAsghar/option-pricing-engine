add_executable(pricer-api
  main.cpp
  routes.cpp)

target_link_libraries(pricer-api PRIVATE pricer_json pricer_market httplib::httplib)
target_include_directories(pricer-api PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(pricer-api PRIVATE ${PRICER_WARNINGS})
pricer_link_runtime(pricer-api)

find_package(OpenSSL)
if(OpenSSL_FOUND)
  target_compile_definitions(pricer-api PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
  target_link_libraries(pricer-api PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
