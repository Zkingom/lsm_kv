#!/usr/bin/env bash
# 一键编译并跑全部测试。
#   用法：scripts/build_and_test.sh [构建目录] [额外 cmake 参数...]
#   例子：scripts/build_and_test.sh build-tsan -DKV_SANITIZE=thread -DCMAKE_BUILD_TYPE=Debug
# 如果机器不能联网下载 GoogleTest，可以先手动下载源码，再加参数
#   -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest-1.14.0
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${1:-$repo_dir/build}"
shift || true

cmake -S "$repo_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$build_dir" -j"$(nproc)"
cd "$build_dir"
ctest --output-on-failure
