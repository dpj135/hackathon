#!/usr/bin/env python3
"""Generate fresh combinations after a source freeze; never select policy parameters from them."""
import argparse
import copy
import hashlib
import json
import random
from pathlib import Path

FAMILIES = ['arrival_ramp', 'staggered_hotsets', 'ttl_turnover', 'asymmetric_reuse']

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--code', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source-sha256', required=True)
    parser.add_argument('--seeds', type=int, nargs='+', default=[271013, 271019, 271027, 271043, 271057, 271067])
    args = parser.parse_args()
    assert hashlib.sha256((args.code / 'player/solution.cpp').read_bytes()).hexdigest() == args.source_sha256
    instance = json.loads((args.code / 'data/promote_hbm_low.json').read_text())['instance']
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = []
    for seed in args.seeds:
        for family in FAMILIES:
            rng = random.Random(seed)
            inst = copy.deepcopy(instance)
            inst['instance_id'] = f'{family}_{seed}'
            count = rng.choice([2, 4, 8])
            length = rng.choice([65536, 1048576, 4194304])
            active_mib = max(1, (8 * count * length + (1 << 20) - 1) // (1 << 20))
            inst['resources'].update(kvcache_pool_capacity=max(1, active_mib * rng.choice([1, 2, 4]) // 2),
                d2h_copy_concurrency=rng.choice([1, 2, 10]), h2d_copy_concurrency=rng.choice([8, 64, 1024]))
            if family == 'ttl_turnover':
                inst['fixed_parameters']['promote_ttl_us'] = rng.choice([1500, 7000, 30000])
            events = []
            now = 0
            for batch in range(24):
                if batch:
                    if family == 'arrival_ramp':
                        gap = [70000, 9000, 700, 53, 1700, 40000][batch // 4]
                    elif family == 'ttl_turnover':
                        gap = rng.choice([700, 1100, 3500, 9000])
                    else:
                        gap = rng.choice([97, 900, 2700, 13000])
                    now += gap
                ios = []
                for card in range(8):
                    for index in range(count):
                        epoch = 0
                        if family == 'staggered_hotsets': epoch = (batch + card) // 6
                        if family == 'asymmetric_reuse' and card % 2: epoch = batch
                        unaligned = (index + card + seed) % 3 == 0
                        ios.append(dict(iocb_id=card * count + index + 1, opcode='READV', device_id=card,
                            ssd_offset=epoch * (1 << 40) + card * (1 << 36) + index * (1 << 24), length=length,
                            iovs=[dict(iov_id=7, hbm_addr=(1 << 48) + card * (1 << 36) + index * (1 << 24) + int(unaligned), length=length)]))
                events.append(dict(event='IO_SUBMIT', time_us=now,
                    submit=dict(submit_id=batch + 1, arrival_us=now, iocbs=ios)))
            path = args.output / (inst['instance_id'] + '.json')
            path.write_text(json.dumps(dict(instance=inst, events=events), separators=(',', ':')) + '\n')
            manifest.append(dict(file=path.name, family=family, seed=seed, requests=24 * 8 * count,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest(), source_freeze_sha256=args.source_sha256,
                selection='Generated after source freeze; not used for retuning'))
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Generated', len(manifest), 'frozen-policy holdout inputs')

if __name__ == '__main__':
    main()
