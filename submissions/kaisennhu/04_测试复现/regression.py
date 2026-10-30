#!/usr/bin/env python3
"""Functional tests against the unmodified organizer validator; Python stdlib only."""
import argparse, copy, hashlib, json, random, subprocess, tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--code',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();root=a.code.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
base=json.loads((root/'data/small_aligned.json').read_text())['instance']
# Official hand-written traces retain the legacy alias. Normalize the local test
# template before changing capacity; do not edit any original official input.
if 'mooncake_capacity' in base['resources']:
    base['resources']['kvcache_pool_capacity']=base['resources'].pop('mooncake_capacity')
rows=[]
def run(cmd, timeout=120, ok=True):
    r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=timeout)
    if ok and r.returncode: raise AssertionError(f'{cmd}: {r.stderr[-1500:]}')
    return r

def io(n,ssd,length,device=0,lengths=None,unaligned=False):
    lengths=lengths or [length]
    return dict(iocb_id=n,opcode='READV',device_id=device,ssd_offset=ssd,length=length,
                iovs=[dict(iov_id=k,hbm_addr=2**36+n*2**28+k*2**24+int(unaligned),length=l) for k,l in enumerate(lengths)])
def trace(groups,resources=None,costs=None):
    inst=copy.deepcopy(base);inst['instance_id']='local-regression'
    inst['resources'].update(resources or {});inst['fixed_parameters'].update(costs or {})
    return dict(instance=inst,events=[dict(event='IO_SUBMIT',time_us=t,submit=dict(submit_id=i+1,arrival_us=t,iocbs=items)) for i,(t,items) in enumerate(groups)])
with tempfile.TemporaryDirectory(prefix='nds-regression-') as temp:
    work=Path(temp)
    def check(name,t):
        src=work/f'{name}.json';dst=work/f'{name}.result.json';src.write_text(json.dumps(t))
        run([root/'nds_player',src,dst]);run([root/'nds_validate',src,dst])
        result=json.loads(dst.read_text());rows.append(dict(name=name,status='passed',metrics=result['metrics']))
        return result,src,dst
    result,_,_=check('cross_iov_unaligned',trace([(0,[io(1,513,4680,lengths=[100,4000,580],unaligned=True)])]))
    assert sum(len(o['transfers']) for d in result['schedule_decisions'] for o in d['operations'])>=2
    r,_,_=check('adjacent_direct',trace([(0,[io(i,4096+i*512,512) for i in range(64)])]))
    assert len(r['schedule_decisions'][0]['operations'])<64
    for path in ['aggregate','fallback']:
        costs={'CPU_FALLBACK_FIXED_COST':1000000} if path=='aggregate' else {'CPU_NDS_FIXED_COST':1000000}
        t=trace([(i*100,[io(j,j*16777216+i*2**32,8*2**20,device=j%8,unaligned=True) for j in range(64)]) for i in range(3)],
            resources={'hbm_capacity':1,'ddr_capacity':1,'iocb_capacity':8},costs=costs)
        r,_,_=check(f'{path}_pool_reuse_over_1GiB',t)
        ops=[o for d in r['schedule_decisions'] for o in d['operations']]
        assert any(o['path']==path.upper() for o in ops)
        assert sum(o.get('buffer_bytes',0) for o in ops)>2**30
        assert max(o.get('buffer_bytes',0) for o in ops)<=2**20
    t=trace([(0,[io(i,i*4*2**20,2*2**20+584,unaligned=True) for i in range(5)])],
            resources={'iocb_capacity':1,'hbm_capacity':1,'ddr_capacity':1})
    check('split_slot_capacity_one',t)
    # Slot admission is per IOCB: a merged read needs all owners' slots, a split
    # IOCB can run several reads on one slot, and an empty budget must queue.
    t=trace([(0,[io(i,i*512*1024,512*1024) for i in range(16)])],resources={'iocb_capacity':8})
    r,_,_=check('slot_merge_budget',t)
    assert len([o for d in r['schedule_decisions'] for o in d['operations']])<16
    t=trace([(0,[io(1,0,8*2**20),io(2,16*2**20,4*2**20),io(3,32*2**20,8*2**20,device=1)])],
            resources={'iocb_capacity':2})
    check('slot_uneven_split',t)
    t=trace([(0,[io(i,i*2**20,8*2**20) for i in range(8)]),
             (1,[io(i,64*2**20+i*2**20,8*2**20) for i in range(8)])],resources={'iocb_capacity':4})
    r,_,_=check('slot_table_full',t)
    assert len(r['schedule_decisions'])>=2
    t=trace([(0,[io(1,0,4096),io(2,8*2**20,4096,unaligned=True),io(3,16*2**20,2**20)])],
            resources={'hbm_capacity':1,'iocb_capacity':2})
    r,_,_=check('slot_mixed_paths',t)
    assert len({o['path'] for d in r['schedule_decisions'] for o in d['operations']})>=2
    # Two AGGREGATE groups on different IOCBs share the single slot, so only one
    # can be in flight: a batch with two operations is not overlap evidence.
    t=trace([(0,[io(1,0,4096,unaligned=True),io(2,4096,4096,unaligned=True)])],
            resources={'iocb_capacity':1,'ddr_capacity':1},
            costs={'CPU_FALLBACK_FIXED_COST':1000000})
    r,src,dst=check('serial_two_groups',t)
    assert [o['path'] for d in r['schedule_decisions'] for o in d['operations']]==['AGGREGATE']*2
    baseline=work/'serial_two_groups.baseline.json'
    run([root/'nds_baseline',src,baseline]);run([root/'nds_validate',src,baseline])
    assert r['metrics']['total_cost']<=json.loads(baseline.read_text())['metrics']['total_cost']
    check('overlapping_reads',trace([(0,[io(1,1001,584,unaligned=True),io(2,1100,768,unaligned=True)])]))
    r,src,dst=check('ttl_expiry',trace([(0,[io(1,0,1048576)]),(500,[io(1,0,1048576)]),(8000,[io(1,0,1048576)])],costs={'promote_ttl_us':1000}))
    assert r['metrics']['cache_hits']==0
    saved=dst.read_bytes();run([root/'nds_player',src,dst]);assert dst.read_bytes()==saved
    rows.append(dict(name='deterministic_bytes',status='passed'))
    altered=copy.deepcopy(r)
    completion=next(e for e in altered['events'] if e['event']=='IOCB_COMPLETE')
    completion['completions'][0]['complete_us']+=1
    bad=work/'corrupted.json';bad.write_text(json.dumps(altered))
    assert run([root/'nds_validate',src,bad],ok=False).returncode!=0
    rows.append(dict(name='reject_corrupt_event',status='passed'))
    t=trace([(0,[io(i,4096+i*512,512) for i in range(8)]),(100,[io(1,2**30,4096)])])
    r,_,_=check('online_prefix_a',t);t['events'][1]['submit']['iocbs']=[io(2,2**31,8*2**20,unaligned=True)]
    r2,_,_=check('online_prefix_b',t);assert r['schedule_decisions'][0]==r2['schedule_decisions'][0]
    rows.append(dict(name='future_independent_prefix',status='passed'))
    # Force worthwhile promotions into a one-object cache, then verify LRU eviction.
    groups=[]
    for k in range(3):
        for repeat in range(2): groups.append((k*4000+repeat*1000,[io(1,k*2**20,2**20)]))
    r,_,_=check('lru_capacity',trace(groups,resources={'kvcache_pool_capacity':1},
                                    costs={'CPU_NDS_FIXED_COST':1000}))
    assert r['metrics']['evictions']>=1 and r['metrics']['promote_success']>=2
    for name in ['promote_ssd_mid','promote_hbm_mid','promote_high']:
        src=root/f'data/{name}.json';dst=work/f'{name}.json'
        run([root/'nds_player',src,dst]);run([root/'nds_validate',src,dst]);r=json.loads(dst.read_text())
        paths={x['path'] for x in r['promotion_decisions']}
        # Reject promotion when small-object reuse does not cover its marginal cost.
        if name=='promote_ssd_mid': assert r['metrics']['total_cost'] < 10000
        # Legality comes from the official validator in check(); promotion is
        # optional, so assert cost rather than a particular promotion path.
        baseline=work/f'{name}.baseline.json'
        run([root/'nds_baseline',src,baseline])
        run([root/'nds_validate',src,baseline])
        # 1% tolerance: the planner is a model, not the simulator.
        assert r['metrics']['total_cost'] <= 1.01*json.loads(baseline.read_text())['metrics']['total_cost']
        if name=='promote_hbm_mid': hbm_capable = 'PROMOTE_HBM' in paths
        rows.append(dict(name=name,status='passed',metrics=r['metrics'],promotion_paths=sorted(paths)))
    # Explicit capability coverage: SSD promotion is worthwhile and D2H is slow.
    groups=[(k*2000,[io(1,0,2**20)]) for k in range(4)]
    r,_,_=check('forced_ssd_promotion',trace(groups,resources={'d2h_bw':1},
                                          costs={'CPU_NDS_FIXED_COST':1000}))
    assert 'PROMOTE_SSD' in {x['path'] for x in r['promotion_decisions']}
    assert r['metrics']['cache_hits'] >= 1
    # Explicit capability coverage: the SSD is slow, so a re-read promotion is
    # worse than the D2H copy and HBM promotion must be available.
    groups=[(k*2000,[io(1,0,2**20)]) for k in range(4)]
    r,_,_=check('forced_hbm_promotion',trace(groups,resources={'nvme_bw':1024},
                                          costs={'CPU_NDS_FIXED_COST':1000}))
    assert 'PROMOTE_HBM' in {x['path'] for x in r['promotion_decisions']}
    assert r['metrics']['cache_hits'] >= 1
    rows.append(dict(name='promote_hbm_capable',status='passed' if hbm_capable else 'unused'))
    # TTL test must establish insertion and a hit before testing expiry.
    r,_,_=check('real_ttl_expiry',trace([(t,[io(1,0,2**20)]) for t in [0,500,900,4000]],
                                      costs={'CPU_NDS_FIXED_COST':1000,'promote_ttl_us':1500}))
    assert r['metrics']['promote_success']==1 and r['metrics']['cache_hits']==1
    assert any(d['submit_id']==4 for d in r['schedule_decisions'])
    for seed in range(20):
        rng=random.Random(seed);groups=[]
        for batch in range(3):
            items=[]
            for j in range(16):
                lengths=[rng.choice([1,511,512,584,768,4096,32768,65536]) for _ in range(rng.randint(1,5))]
                items.append(io(j,rng.randrange(8192)*512+rng.randrange(512),sum(lengths),j%8,lengths,True))
            groups.append((batch*7,items))
        check(f'fuzz_seed_{seed}',trace(groups,resources={'iocb_capacity':4,'hbm_capacity':1,'ddr_capacity':1,
              'h2d_copy_concurrency':1,'d2h_copy_concurrency':1,'hbm_copy_concurrency':1}))
(out/'regression.json').write_text(json.dumps({'passed':len(rows),'tests':rows},indent=2)+'\n')
print(f'{len(rows)} regression checks passed')
