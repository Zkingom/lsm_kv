#!/usr/bin/env bash
# 用 ThreadSanitizer 编译并跑全部测试。
#   用法：scripts/run_tsan.sh [构建目录] [额外 cmake 参数...]
# 新内核（vm.mmap_rnd_bits 较大）上 TSan 会报 "unexpected memory mapping"，
# 所以用 setarch -R 让测试进程关闭地址随机化（只影响这个进程，不改系统设置）。
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${1:-$repo_dir/build-tsan}"
shift || true

cmake -S "$repo_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug -DKV_SANITIZE=thread "$@"
cmake --build "$build_dir" -j"$(nproc)"
cd "$build_dir"
KV_FUZZ_OPS="${KV_FUZZ_OPS:-3000}" setarch "$(uname -m)" -R ctest --output-on-failure
