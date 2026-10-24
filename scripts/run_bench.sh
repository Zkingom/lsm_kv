#!/usr/bin/env bash
# 跑一组对比基准：开/关布隆过滤器、不同 MemTable 阈值。
#   用法：scripts/run_bench.sh [构建目录] [条数，默认 500000]
# 50 万条 × (16B 键 + 100B 值) ≈ 58MB 数据，内存占用远低于 1GB。
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${1:-$repo_dir/build}"
num="${2:-500000}"

cmake -S "$repo_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$build_dir" -j"$(nproc)" --target db_bench > /dev/null

run() {
  echo "### $1"
  shift
  "$build_dir/db_bench" --num="$num" "$@"
  echo
}

run "基线：布隆过滤器开，MemTable 4MB" --bloom=1 --memtable_kb=4096
run "关闭布隆过滤器" --bloom=0 --memtable_kb=4096 --benchmarks=fillrandom,readrandom,readmissing
run "MemTable 1MB" --bloom=1 --memtable_kb=1024 --benchmarks=fillrandom,readrandom,scan
run "MemTable 16MB" --bloom=1 --memtable_kb=16384 --benchmarks=fillrandom,readrandom,scan
