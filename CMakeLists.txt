cmake_minimum_required(VERSION 3.20)
project(orderbook CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
# Match the project's runtime library settings on MSVC; no-op elsewhere.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_library(orderbook_core INTERFACE)
target_include_directories(orderbook_core INTERFACE ${CMAKE_SOURCE_DIR}/include)
target_compile_features(orderbook_core INTERFACE cxx_std_20)

add_executable(orderbook src/main.cpp)
target_link_libraries(orderbook PRIVATE orderbook_core)

enable_testing()

add_executable(unit_tests
  tests/scaffold_test.cpp
  tests/test_types.cpp
  tests/test_object_pool.cpp
  tests/test_order_book.cpp
)
target_link_libraries(unit_tests PRIVATE orderbook_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(unit_tests)
