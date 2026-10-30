#!/usr/bin/env python3
"""Transfer-count planning: legal coverage, resource boundaries, costs and scale.

Synthetic cases are invariant tests, not official scores. Keep raw inputs/results
and an optional frozen-policy comparison; never infer a hidden-set guarantee.
"""
import argparse
import copy
import hashlib
import json
import subprocess
import time
from pathlib import Path
from boundaries import iocb

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--code', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--old-player', type=Path)
p.add_argument('--repeat', type=int, default=3)
p.add_argument('--player', type=Path)
a = p.parse_args()
code, out = a.code.resolve(), a.output.resolve()
player = (a.player or code/'nds_player').resolve()
if a.repeat < 1: p.error('--repeat must be positive')
out.mkdir(parents=True, exist_ok=True)
base = json.loads((code/'data/small_aligned.json').read_text())['instance']
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
report = dict(source_sha256=sha(code/'player/solution.cpp'),
              engine_sha256=sha(code/'src/framework.cpp'), cases=[])

def run(name, ios, resources=None, costs=None):
    inst = copy.deepcopy(base)
    inst['instance_id'] = name
    inst['resources'].update(resources or {})
    inst['fixed_parameters'].update(costs or {})
    trace = dict(instance=inst, events=[dict(event='IO_SUBMIT', time_us=0,
        submit=dict(submit_id=1, arrival_us=0, iocbs=ios))])
    src = out/(name+'.input.json')
    src.write_text(json.dumps(trace)+'\n')
    row = dict(name=name, input_sha256=sha(src), runs=[])
    for rep in range(a.repeat):
        dst = out/(name+'.result.json')
        start = time.monotonic()
        q = subprocess.run([player, src, dst], capture_output=True, text=True, timeout=120)
        assert q.returncode == 0, (name, q.stderr)
        seconds = time.monotonic() - start
        subprocess.run([code/'nds_validate', src, dst], check=True, capture_output=True, timeout=120)
        result = json.loads(dst.read_text())
        row['runs'].append(dict(seconds=seconds, output_sha256=sha(dst), metrics=result['metrics']))
        for decision in result['schedule_decisions']:
            for op in decision['operations']:
                reads = [t for t in op['transfers'] if t['type'] in ['DIRECT','SSD_TO_DDR','SSD_TO_HBM_BUFFER']]
                assert all(t['physical_length'] > 0 and t['physical_length'] % 512 == 0 and
                           t['physical_ssd_offset'] % 512 == 0 for t in reads)
                if op['path'] != 'DIRECT':
                    assert op['buffer_bytes'] >= sum(t['physical_length'] for t in reads)
                    ids = {t['transfer_id'] for t in reads}
                    for t in op['transfers']:
                        if t['type'] in ['D2D','H2D']:
                            assert ids <= set(t['depends_on']), (name, 'copy before all reads')
    assert len({r['output_sha256'] for r in row['runs']}) == 1
    if a.old_player:
        dst = out/(name+'.comparison.json')
        subprocess.run([a.old_player.resolve(), src, dst], check=True, capture_output=True, timeout=120)
        subprocess.run([code/'nds_validate', src, dst], check=True, capture_output=True, timeout=120)
        row['comparison_metrics'] = json.loads(dst.read_text())['metrics']
    row['operations'] = sum(len(d['operations']) for d in result['schedule_decisions'])
    row['ssd_transfers'] = sum(t['type'] in ['DIRECT','SSD_TO_DDR','SSD_TO_HBM_BUFFER']
        for d in result['schedule_decisions'] for o in d['operations'] for t in o['transfers'])
    report['cases'].append(row)
    (out/'summary.json').write_text(json.dumps(report, indent=2)+'\n')
    print(name, result['metrics']['total_cost'], flush=True)
    return result

for size in [32768, 131072, 524288, 1<<20, 2<<20, 4<<20, 8<<20]:
    run('single_'+str(size), [iocb(1,0,size)])
for size in [32767,32769,131071,131073,524287,524289]:
    run('unaligned_'+str(size), [iocb(1,100,size,shift=1)])
r = run('one_owner_one_slot', [iocb(1,0,524288)], {'iocb_capacity':1})
assert len(r['schedule_decisions'][0]['operations']) == 1
assert len(r['schedule_decisions'][0]['operations'][0]['transfers']) > 1
run('independent_owners_one_slot', [iocb(i,i*32768,32768) for i in range(16)], {'iocb_capacity':1})
run('mixed_sizes', [iocb(0,0,2<<20)]+[iocb(i,(2<<20)+(i-1)*32768,32768) for i in range(1,4)])
run('balanced_cards', [iocb(c*16+i,c*(1<<30)+i*32768,32768,c) for c in range(8) for i in range(16)])
run('gaps_and_cards', [iocb(i,i*(1<<30),32768,i%8) for i in range(64)])
for path in ['aggregate','fallback']:
    costs = {'CPU_FALLBACK_FIXED_COST':1000000} if path=='aggregate' else {'CPU_NDS_FIXED_COST':1000000}
    run(path+'_staged', [iocb(1,0,524288,shift=1)], costs=costs)
    run(path+'_full_pool', [iocb(i,i*(4<<20),1<<20,i%8,shift=1) for i in range(8)],
        {'hbm_capacity':1,'ddr_capacity':1,'iocb_capacity':2}, costs)
r = run('calls_only', [iocb(1,0,524288)], costs={'time_cost_per_us':0})
assert r['metrics']['cpu_cost'] == 2 and r['metrics']['npu_cost'] == 0
run('expensive_commands', [iocb(1,0,524288)], costs={'CPU_NDS_PER_CMD_COST':10000})
# Thousands of pieces, including many owners and one owner sharing a single slot.
for n in [512,1024]:
    run('large_'+str(n), [iocb(i,i*(8<<20),4<<20,i%8) for i in range(n)])
run('one_owner_4096_pieces', [iocb(1,0,8<<30)], {'iocb_capacity':1})
report['passed'] = len(report['cases'])
(out/'summary.json').write_text(json.dumps(report, indent=2)+'\n')
print(report['passed'], 'planning cases passed')
