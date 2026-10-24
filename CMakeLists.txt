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
  src/bloom.cpp
  src/client.cpp
  src/compaction.cpp
  src/crc32.cpp
  src/db.cpp
  src/db_background.cpp
  src/file.cpp
  src/memtable.cpp
  src/merging_iterator.cpp
  src/protocol.cpp
  src/server.cpp
  src/sstable.cpp
  src/sstable_builder.cpp
  src/sstable_format.cpp
  src/version.cpp
  src/wal.cpp
)
target_include_directories(kv PUBLIC include)
target_link_libraries(kv PUBLIC Threads::Threads)

# 命令行工具：服务端与客户端
add_executable(kv_server tools/kv_server.cpp)
target_link_libraries(kv_server PRIVATE kv)
add_executable(kv_cli tools/kv_cli.cpp)
target_link_libraries(kv_cli PRIVATE kv)

if(KV_BUILD_TESTS)
  enable_testing()
  add_executable(kv_tests
    tests/skiplist_test.cpp
    tests/memtable_test.cpp
    tests/bloom_test.cpp
    tests/compaction_test.cpp
    tests/concurrency_test.cpp
    tests/db_test.cpp
    tests/db_fuzz_test.cpp
    tests/merging_iterator_test.cpp
    tests/server_test.cpp
    tests/sstable_test.cpp
    tests/wal_test.cpp
  )
  target_link_libraries(kv_tests PRIVATE kv GTest::gtest_main)
  include(GoogleTest)
  if(KV_SANITIZE STREQUAL "thread")
    # 新内核上 TSan 需要在关闭 ASLR 的进程里运行（setarch -R），
    # 所以不在编译时跑测试程序做发现，推迟到 ctest 运行时
    gtest_discover_tests(kv_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST)
  else()
    gtest_discover_tests(kv_tests DISCOVERY_TIMEOUT 60)
  endif()
endif()
