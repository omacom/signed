cmake_minimum_required(VERSION 3.21)
project(signed LANGUAGES CXX)

# C++20, which poppler-qt6's headers require.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 DBus Test)
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER REQUIRED IMPORTED_TARGET poppler-qt6)
pkg_check_modules(QPDF REQUIRED IMPORTED_TARGET libqpdf)

# Everything but main.cpp, shared by the app and the tests, so each source
# compiles once and a new file in src/ needs no line here.
file(GLOB signed_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM signed_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)
add_library(signed_core STATIC ${signed_sources})
target_include_directories(signed_core PUBLIC src)
target_link_libraries(signed_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::DBus
  PkgConfig::POPPLER PkgConfig::QPDF)
target_compile_options(signed_core PUBLIC -Wall -Wextra)

add_executable(signed src/main.cpp src/resources.qrc)
target_link_libraries(signed PRIVATE signed_core)

enable_testing()
add_executable(signed_tests tests/signed_tests.cpp)
target_link_libraries(signed_tests PRIVATE signed_core Qt6::Test)
add_test(NAME signed_tests COMMAND signed_tests)

# The command line is tested as an agent uses it: the built binary, no
# display. Python is only a check dependency, so building needs none.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME cli_tests
    COMMAND Python3::Interpreter -m unittest discover -s ${CMAKE_SOURCE_DIR}/tests -p "test_*.py")
  set_tests_properties(cli_tests PROPERTIES ENVIRONMENT SIGNED=$<TARGET_FILE:signed>)
endif()
