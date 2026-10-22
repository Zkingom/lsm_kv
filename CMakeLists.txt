cmake_minimum_required(VERSION 3.20)
project(lsm_kv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
# CMake 默认 Release 是 -O3，按项目约定改成 -O2
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# 可选：-DKV_SANITIZE=thread 或 address。放在拉取 GoogleTest 之前，让 gtest 也被插桩
set(KV_SANITIZE "" CACHE STRING "Sanitizer to enable: thread / address / empty")
if(KV_SANITIZE)
  add_compile_options(-fsanitize=${KV_SANITIZE} -g -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${KV_SANITIZE})
endif()

find_package(Threads REQUIRED)

option(KV_BUILD_TESTS "Build unit tests" ON)
if(KV_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

# 警告选项放在 FetchContent 之后，只作用于我们自己的代码
add_compile_options(-Wall -Wextra -Wpedantic)

add_library(kv STATIC
  src/crc32.cpp
  src/db.cpp
  src/file.cpp
  src/memtable.cpp
  src/wal.cpp
)
target_include_directories(kv PUBLIC include)
target_link_libraries(kv PUBLIC Threads::Threads)

if(KV_BUILD_TESTS)
  enable_testing()
  add_executable(kv_tests
    tests/skiplist_test.cpp
    tests/memtable_test.cpp
    tests/db_test.cpp
    tests/wal_test.cpp
  )
  target_link_libraries(kv_tests PRIVATE kv GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(kv_tests DISCOVERY_TIMEOUT 60)
endif()
