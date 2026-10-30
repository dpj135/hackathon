#!/usr/bin/env python3
"""Measure the submitted policy and official greedy with the same timing wrapper."""
import argparse
import hashlib
import json
import os
import platform
import re
import statistics
import subprocess
import time
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--code', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--repeat', type=int, default=3)
    p.add_argument('--cpu', type=int, default=min(os.sched_getaffinity(0)))
    p.add_argument('--cases', nargs='+', help='Public input stems; default: all 39')
    a = p.parse_args()
    if a.repeat < 1 or a.cpu not in os.sched_getaffinity(0):
        p.error('repeat must be positive; cpu must be available')
    code, out, here = a.code.resolve(), a.output.resolve(), Path(__file__).resolve().parent
    out.mkdir(parents=True, exist_ok=True)
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    flags = ['-O2', '-std=c++17', '-Wall', '-Wextra', '-Wpedantic']
    sources = {'strategy': code/'player/solution.cpp', 'greedy': here/'greedy_policy.cpp'}
    report = dict(repeat=a.repeat, warmup_per_input=1, cpu=a.cpu,
                  platform=platform.platform(),
                  compiler=subprocess.check_output(['g++', '--version'], text=True).splitlines()[0],
                  cpu_model=next(line.split(':', 1)[1].strip() for line in
                                 Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name')),
                  flags=flags, source_sha256={k:digest(v) for k,v in sources.items()},
                  engine_sha256=digest(code/'src/framework.cpp'),
                  entry_sha256=digest(code/'src/player_main.cpp'),
                  timing_wrapper_sha256=digest(here/'profile_main.cpp'), binary_sha256={}, cases=[])
    start = time.perf_counter()
    for label, source in sources.items():
        cmd = ['g++', *flags, '-I'+str(code/'include'), str(code/'src/json.cpp'),
               str(code/'src/framework.cpp'), str(source), str(here/'profile_main.cpp'), '-o', str(out/label)]
        with (out/(label+'.build.log')).open('w') as log:
            subprocess.run(cmd, check=True, stdout=log, stderr=subprocess.STDOUT)
        report['binary_sha256'][label] = digest(out/label)
    report['build_seconds'] = time.perf_counter() - start
    os.sched_setaffinity(0, {a.cpu})
    env = dict(os.environ, NDS_POLICY_MEMORY_LIMIT_MB='16384')
    cases = json.loads((code/'suites/public_suite.json').read_text())['cases']
    cases = [c for c in cases if not a.cases or Path(c['file']).stem in a.cases]
    if not cases or (a.cases and len(cases) != len(set(a.cases))):
        p.error('unknown or duplicate input names')
    for case in cases:
        src = code/'data'/case['file']
        folder = out/src.stem
        folder.mkdir(exist_ok=True)
        row = dict(case=case['id'], file=src.name, input_sha256=digest(src), runs=[], warmup=[])
        expected = {}
        for rep in range(-1, a.repeat):
            for label in (['strategy','greedy'] if rep % 2 == 0 else ['greedy','strategy']):
                dest = folder/f'{label}.{rep}.json'
                start = time.perf_counter()
                run = subprocess.run(['/usr/bin/time', '-f', 'NDS_PROCESS_RSS_KIB %M',
                                      out/label, src, dest], env=env, check=True,
                                     capture_output=True, text=True, timeout=case['timeout_sec'])
                wall = time.perf_counter() - start
                (folder/f'{label}.{rep}.log').write_text(run.stderr)
                replay = time.perf_counter()
                subprocess.run([code/'nds_validate', src, dest], check=True,
                               capture_output=True, timeout=case['timeout_sec'])
                replay = time.perf_counter() - replay
                output_hash = digest(dest)
                if label not in expected:
                    expected[label] = output_hash
                assert output_hash == expected[label], (case['id'],label,rep,'output changed')
                timing = re.search(r'^NDS_POLICY_TIMING (.+)$', run.stderr, re.M)
                peak = re.search(r'^NDS_POLICY_PEAK_KB (\d+)$', run.stderr, re.M)
                rss = re.search(r'^NDS_PROCESS_RSS_KIB (\d+)$', run.stderr, re.M)
                assert timing and peak and rss, run.stderr
                record = dict(label=label, repetition=rep, wall_seconds=wall, validator_seconds=replay,
                              policy_peak_kib=int(peak[1]), process_peak_kib=int(rss[1]),
                              output_sha256=output_hash, **json.loads(timing[1]))
                row['warmup' if rep < 0 else 'runs'].append(record)
                if rep == 0:
                    row.setdefault('metrics',{})[label] = json.loads(dest.read_text())['metrics']
                else:
                    dest.unlink()
        row['summary'] = {}
        for label in sources:
            runs = [r for r in row['runs'] if r['label']==label]
            row['summary'][label] = {key:statistics.median(r[key] for r in runs)
                for key in ['wall_seconds','policy_total_ms','max_on_submit_ms','max_on_complete_ms']}
            row['summary'][label].update({key:max(r[key] for r in runs)
                for key in ['policy_peak_kib','process_peak_kib']})
        report['cases'].append(row)
        (out/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
        print(case['id'],row['summary'],flush=True)
    report['totals'] = {label:{key:sum(c['summary'][label][key] for c in report['cases'])
        for key in ['wall_seconds','policy_total_ms']} for label in sources}
    report['all_valid'] = report['all_repeated_outputs_identical'] = True
    assert all(digest(v)==report['source_sha256'][k] for k,v in sources.items())
    (out/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS',len(cases),'inputs;',report['totals'],flush=True)


if __name__ == '__main__':
    main()
