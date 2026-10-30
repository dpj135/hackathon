#!/usr/bin/env python3
"""Deterministic, extensible non-public workloads. Uses the official generator.

Families/seed lists are declared before candidate selection. Development and
holdout are separate; never select thresholds using the holdout results.
"""
import argparse
import hashlib
import json
import random
import subprocess
from pathlib import Path

FAMILIES = [
    ('cold', ['--preset', 'no-reuse']),
    ('mixed', ['--preset', 'balanced']),
    ('fragments', ['--preset', 'fragmented', '--iovs-max', '24']),
    ('dense', ['--preset', 'aligned', '--sizes', '512', '--adjacent-rate', '1', '--reuse-rate', '0']),
    ('staging', ['--preset', 'aligned', '--sizes', '32768', '--hbm-unaligned', '--adjacent-rate', '1', '--reuse-rate', '0']),
    ('hot4k', ['--preset', 'aligned', '--sizes', '4096', '--cycle-per-card', '8']),
    ('hot64k', ['--preset', 'aligned', '--sizes', '65536', '--cycle-per-card', '8']),
    ('hot1m', ['--preset', 'aligned', '--sizes', '1048576', '--cycle-per-card', '8']),
    ('hot8m', ['--preset', 'aligned', '--sizes', '8388608', '--cycle-per-card', '4']),
    ('cache_churn', ['--preset', 'reuse-hot', '--mooncake-capacity-mb', '2']),
    ('pool_pressure', ['--preset', 'buffer-pressure', '--iocb-capacity', '1', '--h2d-concurrency', '1', '--d2d-concurrency', '1']),
    ('short_ttl', ['--preset', 'reuse-hot', '--promote-ttl-us', '700']),
    ('queue_overlap', ['--preset', 'reuse-hot', '--arrival-gap-us', '1', '--d2h-concurrency', '1']),
    ('phase_shift', ['--preset', 'reuse-hot']),
    ('bandwidth', ['--preset', 'balanced']),
    ('skewed_cards', ['--preset', 'reuse-hot']),
    ('sparse', ['--preset', 'reuse-hot', '--arrival-gap-us', '1000000']),
    ('bursty', ['--preset', 'reuse-hot', '--arrival-gap-us', '0']),
    ('small_cache_hot', ['--preset', 'aligned', '--sizes', '1048576', '--cycle-per-card', '4', '--mooncake-capacity-mb', '8']),
    ('one_card', ['--preset', 'balanced', '--cards', '1']),
    ('two_cards', ['--preset', 'reuse-hot', '--cards', '2']),
    ('sixteen_cards', ['--preset', 'reuse-hot', '--cards', '16']),
    ('recovery', ['--preset', 'reuse-hot']),
    ('unaligned_large', ['--preset', 'high-unaligned', '--sizes', '1048577,2097153,8388609', '--iovs', '1']),
]
SEEDS = {'dev': [26102901, 26102903, 26102907],
         'holdout': [26103109, 26110313, 26110717, 26110919]}

def generate(code, dest, split, seeds=None):
    dest.mkdir(parents=True, exist_ok=True)
    records = []
    for seed in seeds or SEEDS[split]:
        for family, options in FAMILIES:
            rng = random.Random(seed)
            name = f'{family}_{seed}'
            path = dest / (name + '.json')
            args = ['--cards', '8', '--balance-cards', '--submits', '12', '--iocbs', '64',
                    '--arrival-gap-us', str(rng.choice([200, 1200, 5000])),
                    '--arrival-jitter-us', str(rng.choice([0, 37, 700])),
                    '--iovs-max', '4', '--seed', str(seed), '--instance-id', name,
                    *options]
            subprocess.run([str(code/'nds_generate'), *args, '-o', str(path)],
                           check=True, stdout=subprocess.DEVNULL)
            trace = json.loads(path.read_text())
            # SDK accepts this alias; normalize only newly generated local data.
            resources = trace['instance']['resources']
            if 'mooncake_capacity' in resources:
                resources['kvcache_pool_capacity'] = resources.pop('mooncake_capacity')
            for b, event in enumerate(trace['events']):
                for io in event['submit']['iocbs']:
                    if family == 'phase_shift' and b >= 6:
                        io['ssd_offset'] += 2**42
                    if family == 'recovery' and 4 <= b < 8:
                        io['ssd_offset'] += (b+1)*2**42
                    if family == 'skewed_cards':
                        io['device_id'] = 0 if io['iocb_id'] % 8 else 7
                if family == 'bursty':
                    t = (b // 4) * 7000
                    event['time_us'] = event['submit']['arrival_us'] = t
            if family == 'bandwidth':
                for key in ['nvme_bw', 'h2d_bw', 'd2h_bw', 'd2d_bw']:
                    resources[key] = max(1, resources[key] * rng.choice([1, 2, 4]) // rng.choice([1, 4, 16]))
            path.write_text(json.dumps(trace, separators=(',', ':'))+'\n')
            records.append(dict(file=path.name, family=family, seed=seed, split=split,
                                sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                                generator_arguments=args,
                                non_contest_topology=family in ['skewed_cards','one_card','two_cards','sixteen_cards']))
    (dest/'manifest.json').write_text(json.dumps(records, indent=2)+'\n')
    print(f'{split}: {len(records)} inputs in {dest}', flush=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--code', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--split', choices=SEEDS, required=True)
    parser.add_argument('--seeds', type=int, nargs='+', help='Additional seeds for future runs')
    args = parser.parse_args()
    generate(args.code.resolve(), args.output.resolve(), args.split, args.seeds)
