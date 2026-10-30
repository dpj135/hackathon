#!/usr/bin/env python3
"""Deterministic cache-admission workloads; generated from the public schema."""
import argparse
import copy
import hashlib
import json
from pathlib import Path

MiB = 1 << 20

def generate(code, output):
    instance = json.loads((code / 'data/promote_hbm_low.json').read_text())['instance']
    output.mkdir(parents=True, exist_ok=True)
    manifest = []

    def emit(name, family, patterns, gap, capacity, unaligned=False, tail=False):
        inst = copy.deepcopy(instance)
        inst['instance_id'] = name
        inst['resources']['kvcache_pool_capacity'] = capacity
        events = []
        for batch, pattern in enumerate(patterns):
            ios = []
            for index, (card, key) in enumerate(pattern):
                offset = key * 2 * MiB + card * (1 << 36)
                address = (card + 1) * (1 << 36) + index * 2 * MiB
                if tail and batch == len(patterns) - 1:
                    offset += 10000 * MiB
                elif unaligned:
                    address += 1
                ios.append(dict(device_id=card, iocb_id=index + 1, opcode='READV',
                    ssd_offset=offset, length=MiB,
                    iovs=[dict(iov_id=0, hbm_addr=address, length=MiB)]))
            events.append(dict(event='IO_SUBMIT', time_us=batch * gap,
                submit=dict(submit_id=batch + 1, arrival_us=batch * gap, iocbs=ios)))
        trace = dict(instance=inst, events=events)
        path = output / (name + '.json')
        path.write_text(json.dumps(trace, separators=(',', ':')) + '\n')
        keys = {(io['device_id'], io['ssd_offset'], io['length'])
                for event in events for io in event['submit']['iocbs']}
        manifest.append(dict(file=path.name, family=family, sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            requests=sum(len(e['submit']['iocbs']) for e in events), unique_cache_keys=len(keys),
            capacity_mib=capacity, arrival_gap_us=gap, origin='local protocol construction'))

    for capacity, cycle in [(1, 2), (1, 8), (8, 8), (8, 9), (8, 1)]:
        emit(f'cycle_cap{capacity}_percard{cycle}', 'small_pool_cycle',
             [[(card, batch % cycle) for card in range(8)] for batch in range(80)], 2000, capacity)
    for per_card in [1, 8, 32]:
        for gap in [1000, 2000, 5000]:
            patterns = [[(card, phase * 10000 + key) for card in range(8) for key in range(per_card)]
                        for phase, rounds in [(0, 6), (1, 12)] for _ in range(rounds)]
            emit(f'shift_k{per_card}_gap{gap}', 'hotspot_replacement', patterns, gap, 8 * per_card)
    for repeats in [2, 3, 6]:
        emit(f'sparse_repeats{repeats}', 'sparse_reuse',
             [[(card, key) for card in range(8) for key in range(8)] for _ in range(repeats)], 100000, 1024)
    for unaligned in [False, True]:
        emit('idle_with_tail_' + ('unaligned' if unaligned else 'aligned'), 'idle_command_savings',
             [[(card, 0) for card in range(8)] for _ in range(101)], 100000, 8, unaligned, True)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--code', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print('Generated', len(generate(args.code, args.output)), 'admission workloads')

if __name__ == '__main__':
    main()
