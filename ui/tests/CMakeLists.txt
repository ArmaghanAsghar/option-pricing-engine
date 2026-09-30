# Chart builders are pure functions from JSON to a QChart, so they can be
# tested headlessly. No pixel comparison: the assertions are about series
# counts, axis ranges, and how bad input is handled.
qt_add_executable(pricer_ui_tests test_chart_builders.cpp)

target_link_libraries(pricer_ui_tests PRIVATE
  Qt6::Widgets Qt6::Charts Qt6::Test)

target_include_directories(pricer_ui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_sources(pricer_ui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../chart_builders.cpp)
target_compile_options(pricer_ui_tests PRIVATE -Wall -Wextra)

add_test(NAME ui_chart_builders COMMAND pricer_ui_tests)
# Charts need a QGuiApplication but not a real display.
set_tests_properties(ui_chart_builders PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
