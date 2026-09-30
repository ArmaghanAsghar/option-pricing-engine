# Qt 6 desktop front end. Off by default so the Docker build never needs Qt.
# Configure with -DPRICER_BUILD_UI=ON and a CMAKE_PREFIX_PATH pointing at Qt.
find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Charts Network Sql Test)

qt_standard_project_setup()

qt_add_executable(pricer-ui
  main.cpp
  main_window.cpp
  api_client.cpp
  watchlist.cpp
  chart_builders.cpp
  pricing_panel.cpp)

target_link_libraries(pricer-ui PRIVATE
  Qt6::Widgets
  Qt6::Charts
  Qt6::Network
  Qt6::Sql
  pricer_core)

target_include_directories(pricer-ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Qt's own headers do not survive -Wconversion cleanly, so the UI uses a
# narrower warning set than the engine.
target_compile_options(pricer-ui PRIVATE -Wall -Wextra)

set_target_properties(pricer-ui PROPERTIES
  MACOSX_BUNDLE TRUE
  MACOSX_BUNDLE_BUNDLE_NAME "Option Pricer"
  MACOSX_BUNDLE_GUI_IDENTIFIER aero.joby.pricer
  WIN32_EXECUTABLE TRUE)

if(PRICER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
