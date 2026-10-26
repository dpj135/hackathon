#!/usr/bin/env bash
# Score the no-PROMOTE control on the public suite and check that the
# promote-hit cases pull its automated score below a promote-aware player.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT

g++ -O2 -std=c++17 -Wall -Wextra -I"$root/include" \
  "$root/src/json.cpp" "$root/src/framework.cpp" "$root/src/player_main.cpp" \
  "$root/player/no_promote.cpp" -o "$work/nds_player"
g++ -O2 -std=c++17 -Wall -Wextra -I"$root/include" \
  "$root/src/json.cpp" "$root/src/framework.cpp" "$root/src/validate_main.cpp" \
  -o "$work/nds_validate"

python3 "$root/scripts/score_suite.py" \
  --player "$work/nds_player" \
  --validate "$work/nds_validate" \
  --suite "$root/suites/public_suite.json" \
  --data-dir "$root/data" \
  --frozen "$root/suites/frozen_costs.json" \
  --baseline "$root/suites/public_baseline.json" \
  --label "no-promote control" \
  --output "$work/score.json"

python3 - "$root" "$work/score.json" <<'PY'
import json, subprocess, sys
root, score_path = sys.argv[1:]
score = json.load(open(score_path, encoding="utf-8"))
if score["valid_cases"] != score["case_count"]:
    raise SystemExit(
        f"no-promote control was not valid on every case: "
        f"{score['valid_cases']}/{score['case_count']}"
    )
if not score["automated"] <= 80:
    raise SystemExit(
        f"no-promote control should land at or below 80/90, got {score['automated']}"
    )
suite = json.load(open(f"{root}/suites/public_suite.json", encoding="utf-8"))
files = {case["id"]: case["file"] for case in suite["cases"]}
watched = [
    "promote_low_aligned",
    "promote_low_unaligned",
    "promote_mid",
    "promote_high",
]
by_id = {case["id"]: case for case in score["cases"]}
for case_id in watched:
    case = by_id.get(case_id)
    if case is None or case.get("status") != "ok":
        raise SystemExit(f"{case_id} did not score: {case}")
    trace = json.load(open(f"{root}/data/{files[case_id]}", encoding="utf-8"))
    iocbs = sum(len(event["submit"]["iocbs"]) for event in trace["events"])
    metrics = json.loads(subprocess.check_output(
        [f"{root}/nds_baseline", f"{root}/data/{files[case_id]}", "/tmp/nds_promote_check.json"],
        text=True,
    ))
    hits = metrics["cache_hits"]
    if case_id == "promote_high":
        # 池子装不下工作集：命中很少，LRU 驱逐很多，不提升的成本不高于无脑提升。
        if hits >= 0.05 * iocbs or metrics["evictions"] < 1000 or case["total_cost"] > metrics["total_cost"]:
            raise SystemExit(
                f"{case_id} does not show capacity eviction: "
                f"hits {hits}/{iocbs} evictions {metrics['evictions']} "
                f"no-promote J {case['total_cost']} greedy J {metrics['total_cost']}"
            )
        print(
            f"  {case_id}: hit {hits}/{iocbs} ({hits / iocbs:.1%})  "
            f"evict {metrics['evictions']}  ratio {case['ratio']:.3f}"
        )
        continue
    if case.get("ratio", 1) >= 0.80:
        raise SystemExit(f"{case_id} does not separate promote: {case}")
    if hits < 0.70 * iocbs:
        raise SystemExit(f"{case_id} hit rate {hits}/{iocbs} is below 70%")
    print(f"  {case_id}: hit {hits}/{iocbs} ({hits / iocbs:.1%})  ratio {case['ratio']:.3f}")
print(
    f"distinguish OK  no-promote {score['automated']:.2f}/90  "
    f"valid {score['valid_cases']}/{score['case_count']}"
)
PY
