#!/usr/bin/env python3
"""Independent protocol-level test design; does not call the official generator."""
import argparse,copy,hashlib,json,random
from pathlib import Path

p=argparse.ArgumentParser();p.add_argument('--code',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();out=a.output;out.mkdir(parents=True,exist_ok=True)
base=json.loads((a.code/'data/small_aligned.json').read_text())['instance']
if 'mooncake_capacity' in base['resources']:base['resources']['kvcache_pool_capacity']=base['resources'].pop('mooncake_capacity')
families=['zipf_mixed','zipf_phase','burst_zipf','dense_staging','two_hit_tail','persistent_hot','hot_scan','ragged_ranges']
seeds=[41029,41039,41047];manifest=[]
for seed in seeds:
    for family in families:
        rng=random.Random(seed);inst=copy.deepcopy(base);name=f'{family}_{seed}';inst['instance_id']=name
        per_card={41029:8,41039:32,41047:128}[seed]
        batches=2 if family=='two_hit_tail' else 24
        if family in ['zipf_phase','hot_scan']:inst['resources']['kvcache_pool_capacity']=64
        events=[];clock=0;cursor=0
        object_sizes=[rng.choice([512,4096,12288,65536,1048576]) for _ in range(64)]
        for b in range(batches):
            if b:
                clock+=({41029:80,41039:1700,41047:9000}[seed] if family!='burst_zipf' else (50000 if b%6==0 else 0))
            positions=[]
            for j in range(per_card):
                key=rng.choices(range(64),weights=[1/(k+1)**1.1 for k in range(64)])[0]
                length=object_sizes[key];off=key*2**26;unaligned=bool(key%3==0)
                if family=='zipf_phase':off+=(b//6)*2**44
                if family in ['two_hit_tail','persistent_hot']:
                    length={41029:4096,41039:65536,41047:1048576}[seed];off=j*2**26;unaligned=False
                if family=='dense_staging':
                    length={41029:8192,41039:16384,41047:4096}[seed];off=(b*per_card+j)*length;unaligned=True
                if family=='hot_scan' and b%6 in [3,4,5]:off=(100000+b*per_card+j)*2**26
                if family=='ragged_ranges':
                    length=rng.choice([511,512,513,4095,4096,4097,32767,32768,32769]);off=2**34+cursor+rng.choice([0,1,511]);cursor+=length;unaligned=True
                # Preserve exactly the same per-card layout and dispersion.
                parts=[length] if length<1536 or family in ['dense_staging','two_hit_tail','persistent_hot'] else [512,length-1024,512]
                positions.append((off,length,unaligned,parts))
            ios=[]
            for card in range(8):
                for j,(off,length,unaligned,parts) in enumerate(positions):
                    ident=card*per_card+j
                    ios.append(dict(iocb_id=ident,opcode='READV',device_id=card,ssd_offset=off,length=length,
                        iovs=[dict(iov_id=k*7+2,hbm_addr=2**45+card*2**39+ident*2**26+k*2**23+int(unaligned),length=v) for k,v in enumerate(parts)]))
            events.append(dict(event='IO_SUBMIT',time_us=clock,submit=dict(submit_id=b+1,arrival_us=clock,iocbs=ios)))
        trace=dict(instance=inst,events=events);path=out/(name+'.json');path.write_text(json.dumps(trace,separators=(',',':'))+'\n')
        manifest.append(dict(file=path.name,family=family,seed=seed,sha256=hashlib.sha256(path.read_bytes()).hexdigest(),requests=8*per_card*batches,balanced_eight_cards=True,independent_generator=True))
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print('Generated',len(manifest),'independently designed inputs')
