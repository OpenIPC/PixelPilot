# CMakeLists.txt — build + run the unit test
#
# Requires CMake ≥ 3.14 (for FetchContent) and a C++17 toolchain.

cmake_minimum_required(VERSION 3.14)
project(BufferedPacketQueueTests LANGUAGES CXX)

# ---------- Toolchain basics -------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS  OFF)

# ---------- GoogleTest (fetched at configure time) ---------------------------
include(FetchContent)

FetchContent_Declare(
  googletest
  URL  https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# Keep GoogleTest from messing with CRT flags on MSVC
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

# ---------- Test executable --------------------------------------------------
add_executable(queue_test
    BufferedPacketqueue_test.cpp
)

target_include_directories(queue_test PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../
)
target_link_libraries(queue_test
    GTest::gtest_main
)

# Discover and register the test with CTest
include(GoogleTest)
gtest_discover_tests(queue_test)
