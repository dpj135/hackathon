#!/usr/bin/env bash
# Compile the always-FALLBACK example and score it on the public suite.
# A correct kit reports every case valid and an automated score below the starter.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT

g++ -O2 -std=c++17 -Wall -Wextra -I"$root/include" \
  "$root/src/json.cpp" "$root/src/framework.cpp" "$root/src/player_main.cpp" \
  "$root/player/simple_fallback.cpp" -o "$work/nds_player"
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
  --label "always-fallback example" \
  --output "$work/score.json"

python3 - "$work/score.json" <<'PY'
import json, sys
score = json.load(open(sys.argv[1], encoding="utf-8"))
if score["valid_cases"] != score["case_count"]:
    raise SystemExit(
        f"example policy was not valid on every case: {score['valid_cases']}/{score['case_count']}"
    )
if not score["automated"] < 80:
    raise SystemExit(f"example policy should score below 80, got {score['automated']}")
print(f"example OK  valid {score['valid_cases']}/{score['case_count']}  automated {score['automated']:.2f}/90")
PY
