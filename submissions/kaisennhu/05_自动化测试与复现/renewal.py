#!/usr/bin/env python3
"""Protocol-level stochastic locality processes, independent of the official generator."""
import argparse
import copy
import hashlib
import json
import random
from pathlib import Path

FAMILIES=['renewal_hotspot','walking_hotspot','random_epochs','size_heat','barrier_bursts','sparse_revisits']

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--code',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--seeds',type=int,nargs='+',default=[9491,9497,9503,9511])
    p.add_argument('--submits',type=int,default=24);p.add_argument('--per-card',type=int,default=16)
    a=p.parse_args()
    if min(a.submits,a.per_card)<1:p.error('counts must be positive')
    a.output.mkdir(parents=True,exist_ok=True)
    base=json.loads((a.code/'data/small_aligned.json').read_text())['instance']
    if 'mooncake_capacity' in base['resources']:
        base['resources']['kvcache_pool_capacity']=base['resources'].pop('mooncake_capacity')
    records=[]
    for seed in a.seeds:
        for family in FAMILIES:
            rng=random.Random(seed);inst=copy.deepcopy(base);name=f'{family}_{seed}';inst['instance_id']=name
            inst['resources']['kvcache_pool_capacity']=rng.choice([32,96,192,384])
            lengths=[rng.choice([513,7168,40960,786432,1572864,5*2**20,7*2**20]) for _ in range(64)]
            weights=[1/(k+1) for k in range(64)]
            if family=='size_heat':weights=[1/(v**.5) for v in lengths]
            events=[];now=0;anchor=0;epoch_left=0
            for b in range(a.submits):
                if b:
                    gap=max(1,min(45000,int(rng.expovariate(1/6000))))
                    if family=='barrier_bursts':gap=rng.randint(70000,150000) if b%6==0 else 0
                    now+=gap
                if family=='walking_hotspot':anchor=(anchor+rng.choice([-2,-1,1,2]))%64
                if family=='random_epochs':
                    if epoch_left==0:anchor=rng.randrange(64);epoch_left=rng.randint(3,8)
                    epoch_left-=1
                pattern=[]
                for j in range(a.per_card):
                    key=rng.choices(range(64),weights=weights)[0]
                    if family in ['walking_hotspot','random_epochs']:key=(anchor+rng.randrange(16))%64
                    off=key*2**27+int(key%5==0)
                    if family=='sparse_revisits' and rng.random()<.7:off+=(b*a.per_card+j+1)*2**35
                    length=lengths[key]
                    parts=[length] if key%2 else [257,length-257]
                    pattern.append((off,length,parts,key))
                ios=[]
                for card in range(8):
                    for j,(off,length,parts,key) in enumerate(pattern):
                        ios.append(dict(iocb_id=card*a.per_card+j,opcode='READV',device_id=card,ssd_offset=off,length=length,
                            iovs=[dict(iov_id=5*k+3,hbm_addr=2**48+card*2**42+j*2**30+k*2**28+int(key%5==0),length=v)
                                  for k,v in enumerate(parts)]))
                events.append(dict(event='IO_SUBMIT',time_us=now,submit=dict(submit_id=b+1,arrival_us=now,iocbs=ios)))
            dest=a.output/(name+'.json');dest.write_text(json.dumps(dict(instance=inst,events=events),separators=(',',':'))+'\n')
            records.append(dict(file=dest.name,family=family,seed=seed,sha256=hashlib.sha256(dest.read_bytes()).hexdigest(),
                requests=8*a.per_card*a.submits,balanced_eight_cards=True,independent_generator=True))
    (a.output/'manifest.json').write_text(json.dumps(records,indent=2)+'\n');print('Generated',len(records),'stochastic locality inputs.')

if __name__=='__main__':main()
