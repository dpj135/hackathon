#!/usr/bin/env python3
"""Independent protocol workloads; no call to the organizer's generator."""
import argparse
import copy
import hashlib
import json
import random
from pathlib import Path

FAMILIES = ['size_rotation', 'slot_pressure', 'alternating_bands',
            'burst_then_idle', 'cache_visibility', 'hot_cold_switch']

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--code', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--seeds', type=int, nargs='+', default=[9301, 9311, 9323, 9337])
    p.add_argument('--submits', type=int, default=20)
    p.add_argument('--per-card', type=int, default=16)
    a = p.parse_args()
    if min(a.submits, a.per_card) < 1: p.error('counts must be positive')
    a.output.mkdir(parents=True, exist_ok=True)
    base = json.loads((a.code/'data/small_aligned.json').read_text())['instance']
    if 'mooncake_capacity' in base['resources']:
        base['resources']['kvcache_pool_capacity'] = base['resources'].pop('mooncake_capacity')
    records = []
    for seed in a.seeds:
        for family in FAMILIES:
            rng = random.Random(seed)
            inst = copy.deepcopy(base)
            inst['instance_id'] = f'{family}_{seed}'
            r = inst['resources']
            r['kvcache_pool_capacity'] = rng.choice([16, 64, 256])
            if family == 'slot_pressure':
                r.update(iocb_capacity=rng.choice([8, 32, 64]), hbm_capacity=rng.choice([1, 16, 64]),
                         ddr_capacity=128, hbm_copy_concurrency=rng.choice([1, 4, 16]),
                         h2d_copy_concurrency=rng.choice([1, 4, 16]), d2h_copy_concurrency=2)
            lengths = [rng.choice([512, 8192, 130560, 3*2**20, 6*2**20, 8*2**20]) for _ in range(a.per_card)]
            events = []
            now = 0
            for b in range(a.submits):
                if b:
                    gap = rng.choice([0, 37, 211, 5000, 17000])
                    if family == 'burst_then_idle': gap = 300000 if b % 5 == 0 else 29
                    now += gap
                positions = []
                for j in range(a.per_card):
                    length = lengths[j]
                    offset = j*2**27 + int(j % 3 == 0)
                    if family == 'size_rotation': length = [4096, 130560, 3*2**20, 6*2**20][(j+b)%4]
                    if family == 'alternating_bands':
                        length = 130560 if b % 2 else 8192
                        offset = (b*a.per_card+j)*(length+512) + 1
                    if family == 'hot_cold_switch' and b % 8 >= 4: offset += (b+1)*2**35
                    if family == 'slot_pressure': offset += (b+1)*2**35
                    parts = [length] if j % 2 else [512, length-512] if length > 512 else [length]
                    positions.append((offset, length, parts))
                ios = []
                for card in range(8):
                    for j, (offset, length, parts) in enumerate(positions):
                        if family == 'cache_visibility' and card % 2: offset += (b+1)*2**35
                        ios.append(dict(iocb_id=card*a.per_card+j, opcode='READV', device_id=card,
                            ssd_offset=offset, length=length,
                            iovs=[dict(iov_id=3*k+1, hbm_addr=2**48+card*2**42+j*2**30+k*2**28+int(j%3==0), length=v)
                                  for k,v in enumerate(parts)]))
                events.append(dict(event='IO_SUBMIT', time_us=now,
                                   submit=dict(submit_id=b+1, arrival_us=now, iocbs=ios)))
            dest = a.output/(inst['instance_id']+'.json')
            dest.write_text(json.dumps(dict(instance=inst, events=events), separators=(',', ':'))+'\n')
            records.append(dict(file=dest.name, family=family, seed=seed,
                sha256=hashlib.sha256(dest.read_bytes()).hexdigest(), requests=8*a.per_card*a.submits,
                balanced_eight_cards=True, independent_generator=True))
    (a.output/'manifest.json').write_text(json.dumps(records, indent=2)+'\n')
    print(f'Generated {len(records)} independent workload inputs.')

if __name__ == '__main__': main()
