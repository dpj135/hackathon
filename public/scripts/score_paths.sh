#!/usr/bin/env bash
# Show that each read path and each promote path changes J.
# The shipped baseline is a schedule that merges contiguous ranges and
# picks PROMOTE_SSD only for small objects when D2H concurrency is 1.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT

g++ -O2 -std=c++17 -Wall -Wextra -I"$root/include" \
  "$root/src/json.cpp" "$root/src/framework.cpp" "$root/src/player_main.cpp" \
  "$root/player/promote_ssd_only.cpp" -o "$work/nds_ssd"

python3 - "$root" "$work/nds_ssd" <<'PY'
import json, subprocess, sys
root, ssd_bin = sys.argv[1:]
baseline = json.load(open(f"{root}/suites/public_baseline.json", encoding="utf-8"))["cases"]
suite = json.load(open(f"{root}/suites/public_suite.json", encoding="utf-8"))
files = {case["id"]: case["file"] for case in suite["cases"]}

def metrics(binary, case_id):
    path = f"{root}/data/{files[case_id]}"
    return json.loads(subprocess.check_output(
        [binary, path, "/tmp/nds_path_check.json"], text=True))

def iocbs(case_id):
    trace = json.load(open(f"{root}/data/{files[case_id]}", encoding="utf-8"))
    return sum(len(event["submit"]["iocbs"]) for event in trace["events"])

read_cases = [
    "read_direct_low", "read_direct_mid", "read_direct_high",
    "read_aggregate_low", "read_aggregate_mid", "read_aggregate_high",
    "read_fallback_low", "read_fallback_mid", "read_fallback_high",
]
for case_id in read_cases:
    greedy = metrics(f"{root}/nds_baseline", case_id)
    reference = int(baseline[case_id]["total_cost"])
    ratio = reference / greedy["total_cost"]
    if ratio > 0.50:
        raise SystemExit(f"{case_id} merge gap is too small: {ratio:.3f}")
    print(f"  {case_id}: reference/greedy {ratio:.3f}  J {reference} / {greedy['total_cost']}")

for case_id in ["promote_hbm_low", "promote_hbm_mid", "promote_hbm_high"]:
    greedy = metrics(f"{root}/nds_baseline", case_id)
    ssd = metrics(ssd_bin, case_id)
    ratio = ssd["total_cost"] / greedy["total_cost"]
    n = iocbs(case_id)
    if ratio < 1.40 or greedy["cache_hits"] < 0.30 * n or ssd["cache_hits"] >= greedy["cache_hits"]:
        raise SystemExit(
            f"{case_id} does not separate PROMOTE_HBM from PROMOTE_SSD: "
            f"ratio {ratio:.3f} hits {greedy['cache_hits']}/{ssd['cache_hits']} of {n}"
        )
    print(
        f"  {case_id}: SSD-only/HBM {ratio:.3f}  "
        f"hits {greedy['cache_hits']} vs {ssd['cache_hits']} / {n}"
    )

for case_id in ["promote_ssd_low", "promote_ssd_mid", "promote_ssd_high"]:
    greedy = metrics(f"{root}/nds_baseline", case_id)
    reference = int(baseline[case_id]["total_cost"])
    if reference >= greedy["total_cost"]:
        raise SystemExit(f"{case_id} reference is not cheaper than PROMOTE_HBM")
    print(f"  {case_id}: reference/HBM {reference / greedy['total_cost']:.3f}")

print("paths OK")
PY
