#!/usr/bin/env python3
"""Adversarial semantics and metamorphic tests against the unmodified SDK.

These tests deliberately include parameter variations outside the scored frozen
cost set. They test invariants/capability, and are never reported as public score.
"""
import argparse
import copy
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

MiB=2**20

def iocb(n, off, length, card=0, split=None, shift=0):
    sizes=split or [length]
    return dict(iocb_id=n,opcode='READV',device_id=card,ssd_offset=off,length=length,
                iovs=[dict(iov_id=17+3*k,hbm_addr=2**40+n*2**30+k*2**24+shift,length=v) for k,v in enumerate(sizes)])

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--code',type=Path,required=True)
    p.add_argument('--player',type=Path)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();code=a.code.resolve();exe=(a.player or code/'nds_player').resolve()
    base=json.loads((code/'data/small_aligned.json').read_text())['instance']
    r=base['resources']
    if 'mooncake_capacity' in r:r['kvcache_pool_capacity']=r.pop('mooncake_capacity')
    rows=[];a.output.mkdir(parents=True,exist_ok=True)
    def trace(groups,resources=None,costs=None):
        instance=copy.deepcopy(base);instance['instance_id']='boundary'
        instance['resources'].update(resources or {});instance['fixed_parameters'].update(costs or {})
        return dict(instance=instance,events=[dict(event='IO_SUBMIT',time_us=t,submit=dict(submit_id=1+b,arrival_us=t,iocbs=ios)) for b,(t,ios) in enumerate(groups)])
    with tempfile.TemporaryDirectory(prefix='nds-boundaries-') as tmp:
        temp=Path(tmp)
        def run(name,t,valid=True):
            src=temp/'input.json';dst=temp/'output.json';src.write_text(json.dumps(t))
            result=subprocess.run([str(exe),str(src),str(dst)],capture_output=True,text=True,timeout=180)
            if not valid:
                assert result.returncode!=0,(name,'invalid input accepted')
                rows.append(dict(name=name,passed=True,expected='rejected',error=result.stderr.strip()))
                return None
            if result.returncode:
                (a.output/(name+'.input.json')).write_bytes(src.read_bytes())
                raise AssertionError((name,result.stderr))
            subprocess.run([str(code/'nds_validate'),str(src),str(dst)],check=True,capture_output=True,timeout=180)
            out=json.loads(dst.read_text());rows.append(dict(name=name,passed=True,input_sha256=hashlib.sha256(src.read_bytes()).hexdigest(),output_sha256=hashlib.sha256(dst.read_bytes()).hexdigest(),metrics=out['metrics']))
            return out
        # Pool and alignment boundaries, many cards, overlapping physical ranges,
        # nonsequential iov IDs and iov boundaries cutting across chunk boundaries.
        lengths=[1,511,512,513,4095,4096,4097,32767,32768,32769,65535,65536,65537,
                 MiB-1,MiB,MiB+1,2*MiB-1,2*MiB,2*MiB+1,8*MiB+1]
        for length in lengths:
            for shift in [0,1]:
                ios=[]
                for j in range(16):
                    parts=[1,length-1] if length>1 and j%2 else [length]
                    ios.append(iocb(j,2**36+(j//8)*2**26+[0,1,511,512][j%4],length,j%8,parts,shift))
                run(f'length_{length}_hbm_{shift}',trace([(0,ios)],{'hbm_capacity':1,'ddr_capacity':1,'iocb_capacity':1,
                        'hbm_copy_concurrency':1,'h2d_copy_concurrency':1,'d2h_copy_concurrency':1}))
        run('overlap_and_duplicate_ranges',trace([(0,[iocb(j,511+(j%4)*128,4097,j%2,shift=1) for j in range(16)])]))
        q=run('same_time_is_not_reuse',trace([(0,[iocb(j,0,MiB,j%8) for j in range(16)]) for _ in range(3)]))
        assert q['metrics']['promote_count']==0
        # Capacities / expiry endpoints are behavioral checks, not tuning examples.
        for length in [MiB-1,MiB,MiB+1]:
            t=trace([(k*2000,[iocb(j,0,length,j) for j in range(8)]) for k in range(5)],
                    {'kvcache_pool_capacity':1},{'CPU_NDS_FIXED_COST':1000})
            q=run(f'cache_size_{length}',t)
            if length>MiB:assert q['metrics']['promote_count']==0
        for ttl in [0,1,499,500,501,1500]:
            q=run(f'ttl_{ttl}',trace([(t,[iocb(j,0,MiB,j) for j in range(8)]) for t in [0,500,900,4000]],
                  costs={'promote_ttl_us':ttl,'CPU_NDS_FIXED_COST':1000}))
            if ttl==0:assert q['metrics']['promote_count']==0
        # Large addresses are valid without allocating memory at those addresses.
        run('large_safe_addresses',trace([(0,[iocb(j,2**60+j*4096,4096,j%8) for j in range(8)])]))
        # The six-request micro-search is bounded by vector work, on both sides.
        for count in [255,256,257]:
            run(f'micro_iov_{count}',trace([(0,[iocb(1,0,count*512,split=[512]*count)])]))
        # Same physical request, different metadata/path name and additive shifts.
        original=trace([(k*2000,[iocb(j,j*4096,4096,j%8,shift=1) for j in range(16)]) for k in range(5)])
        ref=run('transform_reference',original)
        t=copy.deepcopy(original);t['instance']['instance_id']='not-a-public-case';t['generator_metadata']={'seed':999999,'untrusted_name':'high_mixed'}
        q=run('rename_instance_and_metadata',t);assert q==ref
        for transform in ['ssd_shift','hbm_shift','time_shift','request_id_shift','iov_id_shift','json_key_order']:
            t=copy.deepcopy(original)
            for event in t['events']:
                if transform=='time_shift':
                    event['time_us']+=123456;event['submit']['arrival_us']+=123456
                for io in event['submit']['iocbs']:
                    if transform=='ssd_shift':io['ssd_offset']+=2**35
                    if transform=='request_id_shift':io['iocb_id']+=1000
                    for v in io['iovs']:
                        if transform=='hbm_shift':v['hbm_addr']+=2**35
                        if transform=='iov_id_shift':v['iov_id']+=1000
            if transform=='json_key_order':t=json.loads(json.dumps(t,sort_keys=True))
            q=run(transform,t)
            assert q['metrics']==ref['metrics'],transform
        t=copy.deepcopy(original)
        # Preserve first two arrival events, replace the entire unknown suffix.
        for k,event in enumerate(t['events'][2:]):
            event['submit']['iocbs']=[iocb(j,2**42+k*2**30+j*MiB,MiB+1,j%8,shift=1) for j in range(16)]
        q=run('causal_prefix',t)
        assert q['schedule_decisions'][:2]==ref['schedule_decisions'][:2]
        for resource in ['h2d_copy_concurrency','hbm_capacity','nvme_bw','kvcache_pool_capacity']:
            t=copy.deepcopy(original);t['instance']['resources'][resource]=0
            run('reject_zero_'+resource,t,False)
        for invalid in ['zero_length','iov_sum','duplicate_iocb','duplicate_submit','unsorted_time','negative_offset']:
            t=copy.deepcopy(original);io=t['events'][0]['submit']['iocbs'][0]
            if invalid=='zero_length':io['length']=0
            if invalid=='iov_sum':io['iovs'][0]['length']+=1
            if invalid=='duplicate_iocb':t['events'][0]['submit']['iocbs'].append(copy.deepcopy(io))
            if invalid=='duplicate_submit':t['events'][1]['submit']['submit_id']=1
            if invalid=='unsorted_time':t['events'][1]['time_us']=t['events'][1]['submit']['arrival_us']=999999
            if invalid=='negative_offset':io['ssd_offset']=-1
            run('reject_'+invalid,t,False)
    report=dict(passed=len(rows),source_sha256=hashlib.sha256((code/'player/solution.cpp').read_bytes()).hexdigest(),binary_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),tests=rows)
    (a.output/'boundaries.json').write_text(json.dumps(report,indent=2)+'\n')
    print(f'{len(rows)} boundary/metamorphic checks passed')

if __name__=='__main__':main()
