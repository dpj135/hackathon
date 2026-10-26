#!/usr/bin/env bash
set -euo pipefail

generator="${1:-./nds_generate}"
baseline="${2:-./nds_baseline}"
validator="${3:-./nds_validate}"
test_dir="$(mktemp -d)"
trap 'rm -rf -- "$test_dir"' EXIT

common=(
  --submits 3
  --iocbs 12
  --iovs-min 1
  --iovs-max 4
  --reuse-rate 0.4
  --unaligned-rate 0.6
  --adjacent-rate 0.7
  --hbm-fragment-rate 0.8
  --seed 424242
)

"$generator" "${common[@]}" -o "$test_dir/trace-a.json" >/dev/null
"$generator" "${common[@]}" -o "$test_dir/trace-b.json" >/dev/null
cmp "$test_dir/trace-a.json" "$test_dir/trace-b.json"

"$baseline" "$test_dir/trace-a.json" "$test_dir/solution.json" >/dev/null
"$validator" "$test_dir/trace-a.json" "$test_dir/solution.json" >/dev/null

if "$generator" --submits 1 --iocbs 1025 -o "$test_dir/invalid.json" >/dev/null 2>&1; then
  echo "generator accepted more than 1024 iocbs per submit" >&2
  exit 1
fi

if "$generator" --reuse-rate 1.2 -o "$test_dir/invalid.json" >/dev/null 2>&1; then
  echo "generator accepted an invalid probability" >&2
  exit 1
fi

echo "generator determinism and validation smoke test passed"
