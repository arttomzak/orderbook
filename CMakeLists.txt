cmake_minimum_required(VERSION 3.20)
project(orderbook CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
# Once GoogleTest has been fetched once (cached under build/_deps), never
# reach out to check for updates on later reconfigures - keeps offline
# builds working as long as build/ isn't deleted.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
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

# itchcpp (Layer 1 dependency): vendored as a git submodule, pinned by commit.
# Its own CMake builds the itch::itch library target; its tests/examples/
# benchmarks stay off (ITCH_BUILD_* options unset), so we only compile the lib.
add_subdirectory(third_party/itchcpp)

add_executable(orderbook src/main.cpp src/itch_adapter.cpp)
target_link_libraries(orderbook PRIVATE orderbook_core itch::itch)

enable_testing()

add_executable(unit_tests
  tests/scaffold_test.cpp
  tests/test_types.cpp
  tests/test_object_pool.cpp
  tests/test_order_book.cpp
  tests/test_matching_engine.cpp
)
target_link_libraries(unit_tests PRIVATE orderbook_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(unit_tests)
