#!/usr/bin/env bash
# Score player/solution.cpp on the public suite.
#   bash scripts/score.sh            # score against the frozen baseline
#   bash scripts/score.sh --freeze   # replace the shipped reference with this player
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
mode="${1:-score}"
if [[ "$mode" == "--freeze" ]]; then
  python3 "$root/scripts/score_suite.py" \
    --player "$root/nds_player" \
    --validate "$root/nds_validate" \
    --suite "$root/suites/public_suite.json" \
    --data-dir "$root/data" \
    --frozen "$root/suites/frozen_costs.json" \
    --write-baseline "$root/suites/public_baseline.json" \
    --label "public baseline"
else
  python3 "$root/scripts/score_suite.py" \
    --player "$root/nds_player" \
    --validate "$root/nds_validate" \
    --suite "$root/suites/public_suite.json" \
    --data-dir "$root/data" \
    --frozen "$root/suites/frozen_costs.json" \
    --baseline "$root/suites/public_baseline.json" \
    --label public \
    --output "$root/last_public_score.json"
fi
