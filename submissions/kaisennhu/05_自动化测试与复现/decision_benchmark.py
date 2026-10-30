#!/usr/bin/env python3
"""Compare callback/wall time and exact output with a frozen policy source.

Uses the same external timing wrapper and official allocation meter for both
builds. Compilation, warmup and validation are excluded from measured wall time.
"""
import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
import time
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--code', type=Path, required=True)
p.add_argument('--before-source', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--repeat', type=int, default=3)
p.add_argument('--cpu', type=int, default=min(os.sched_getaffinity(0)))
p.add_argument('--cases', nargs='+', help='Public input stems; default: all 39')
a = p.parse_args()
if a.repeat < 1 or a.cpu not in os.sched_getaffinity(0):
    p.error('repeat must be positive and cpu must be in the allowed affinity')
code, out = a.code.resolve(), a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
flags = ['-O2', '-std=c++17', '-Wall', '-Wextra', '-Wpedantic']
report = dict(repeat=a.repeat, cpu=a.cpu, platform=platform.platform(),
              compiler=subprocess.check_output(['g++', '--version'], text=True).splitlines()[0],
              flags=flags, engine_sha256=sha(code/'src/framework.cpp'),
              entry_sha256=sha(code/'src/player_main.cpp'),
              timing_wrapper_sha256=sha(Path(__file__).with_name('profile_main.cpp')),
              source_sha256={}, binary_sha256={}, cases=[])
report['cpu_model'] = next(line.split(':', 1)[1].strip() for line in
                          Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name'))
start = time.perf_counter()
for label, source in [('before', a.before_source.resolve()), ('after', code/'player/solution.cpp')]:
    frozen = out/(label+'.cpp')
    frozen.write_bytes(source.read_bytes())
    report['source_sha256'][label] = sha(frozen)
    cmd = ['g++', *flags, '-I'+str(code/'include'), str(code/'src/json.cpp'),
           str(code/'src/framework.cpp'), str(frozen),
           str(Path(__file__).with_name('profile_main.cpp')), '-o', str(out/label)]
    with (out/(label+'.build.log')).open('w') as log:
        subprocess.run(cmd, check=True, stdout=log, stderr=subprocess.STDOUT)
    report['binary_sha256'][label] = sha(out/label)
report['build_seconds'] = time.perf_counter() - start
os.sched_setaffinity(0, {a.cpu})
env = dict(os.environ, NDS_POLICY_MEMORY_LIMIT_MB='16384')
suite = json.loads((code/'suites/public_suite.json').read_text())['cases']
cases = [c for c in suite if not a.cases or Path(c['file']).stem in a.cases]
assert cases and (not a.cases or len(cases) == len(set(a.cases)))
for case in cases:
    src = code/'data'/case['file']
    row = dict(case=src.stem, input_sha256=sha(src), runs=[])
    folder = out/src.stem
    folder.mkdir(exist_ok=True)
    expected = None
    for rep in range(-1, a.repeat):  # One warmup per binary/input; never drop shared caches.
        labels = ['after', 'before'] if rep % 2 else ['before', 'after']
        for label in labels:
            prefix = folder/(label+'.'+str(rep))
            dest = prefix.with_suffix(prefix.suffix+'.json')
            start = time.perf_counter()
            run = subprocess.run([out/label, src, dest], env=env, check=True,
                                 capture_output=True, text=True, timeout=180)
            wall = time.perf_counter() - start
            prefix.with_suffix(prefix.suffix+'.log').write_text(run.stderr)
            subprocess.run([code/'nds_validate', src, dest], check=True,
                           capture_output=True, timeout=180)
            digest = sha(dest)
            if expected is None:
                expected = digest
            assert digest == expected, (src.stem, label, rep, 'output changed')
            timing = next(json.loads(line.removeprefix('NDS_POLICY_TIMING '))
                          for line in run.stderr.splitlines() if line.startswith('NDS_POLICY_TIMING '))
            if rep >= 0:
                row['runs'].append(dict(label=label, repeat=rep, wall_seconds=wall, **timing))
            if rep != 0:
                dest.unlink()
    row['output_sha256'] = expected
    row['medians'] = {label: {key: statistics.median(r[key] for r in row['runs'] if r['label'] == label)
                     for key in ['wall_seconds', 'policy_total_ms', 'max_on_submit_ms']}
                     for label in ['before', 'after']}
    report['cases'].append(row)
    (out/'summary.json').write_text(json.dumps(report, indent=2)+'\n')
    print(src.stem, row['medians'], flush=True)
report['totals'] = {label: {key: sum(c['medians'][label][key] for c in report['cases'])
                   for key in ['wall_seconds', 'policy_total_ms']} for label in ['before', 'after']}
report['all_outputs_identical'] = True
(out/'summary.json').write_text(json.dumps(report, indent=2)+'\n')
print('PASS', len(cases), 'inputs;', report['totals'], flush=True)
