#!/usr/bin/env python3
"""Verify that a successful scoring process cannot conceal invalid case results."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path

p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
spec=importlib.util.spec_from_file_location('submission_entry',Path(__file__).resolve().parents[1]/'run.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
ok=dict(cases=[dict(id='sample',status='ok',total_cost=100,peak_mb=12)],case_count=1,
        valid_cases=1,memory_limit_mb=16384,performance=10.0,feasibility=20.0)
module.require_valid_score(ok)
checks=[]
for name,change in [
    ('invalid_case_with_successful_process',lambda r:r['cases'][0].update(status='invalid')),
    ('missing_case',lambda r:r.update(case_count=2)),
    ('memory_over_limit',lambda r:r['cases'][0].update(peak_mb=16385)),
    ('resource_check_failure',lambda r:r.update(performance=9.0)),
]:
    report=copy.deepcopy(ok);change(report)
    try:module.require_valid_score(report)
    except ValueError:checks.append(dict(name=name,passed=True))
    else:raise AssertionError('Gate accepted '+name)
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(dict(checks=checks),indent=2)+'\n')
print(f'{len(checks)} scoring failure-gate checks passed')
