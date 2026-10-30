#!/usr/bin/env python3
"""Scale and high-iov probes, with callback timing and independent official replay."""
import argparse
import gzip
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--code',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--submits',type=int,default=100)
    p.add_argument('--iocbs',type=int,default=1000)
    p.add_argument('--seed',type=int,default=26102929)
    p.add_argument('--timeout',type=int,default=600)
    p.add_argument('--families',nargs='+',default=['mixed','phase','many_iovs','micro','filtered'],choices=['mixed','phase','many_iovs','micro','filtered'])
    a=p.parse_args();code=a.code.resolve();a.output.mkdir(parents=True,exist_ok=True)
    cpu=min(os.sched_getaffinity(0));os.sched_setaffinity(0,{cpu})
    report=dict(source_sha256=hashlib.sha256((code/'player/solution.cpp').read_bytes()).hexdigest(),cpu=cpu,seed=a.seed,
                generated_submits=a.submits,generated_iocbs_per_submit=a.iocbs,cases=[])
    with tempfile.TemporaryDirectory(prefix='nds-stress-') as temp:
        w=Path(temp);trace=w/'trace.json';result=w/'result.json';rss=w/'rss.txt'
        for family in a.families:
            repeats=1 if family in ['mixed','phase'] else 3
            if family in ['mixed','phase']:
                subprocess.run([str(code/'nds_generate'),'--preset','balanced' if family=='mixed' else 'reuse-hot',
                    '--submits',str(a.submits),'--iocbs',str(a.iocbs),'--iovs','1','--balance-cards','--cards','8',
                    '--seed',str(a.seed),'--arrival-gap-us','100','-o',str(trace)],check=True,stdout=subprocess.DEVNULL)
                if family=='phase':
                    t=json.loads(trace.read_text());r=t['instance']['resources'];r.pop('mooncake_capacity',None);r['kvcache_pool_capacity']=16
                    for b,event in enumerate(t['events']):
                        for io in event['submit']['iocbs']:io['ssd_offset']+=(b//max(1,a.submits//4))*2**44
                    trace.write_text(json.dumps(t,separators=(',',':'))+'\n')
            elif family=='many_iovs':
                t=json.loads((code/'data/small_aligned.json').read_text());t['instance']['resources'].update(hbm_capacity=1,ddr_capacity=1,iocb_capacity=1)
                count=65536;io=dict(iocb_id=1,opcode='READV',device_id=0,ssd_offset=1,length=count*8192,
                    iovs=[dict(iov_id=k,hbm_addr=2**40+k*16384+1,length=8192) for k in range(count)])
                t['events']=[dict(event='IO_SUBMIT',time_us=0,submit=dict(submit_id=1,arrival_us=0,iocbs=[io]))]
                trace.write_text(json.dumps(t,separators=(',',':'))+'\n')
            else:
                name='micro_32768_one_byte_iovs' if family=='micro' else 'balanced_cache_filtered_many_iovs'
                fixture=Path(__file__).resolve().parent/'fixtures/stress'/(name+'.json.gz')
                with gzip.open(fixture,'rb') as src,trace.open('wb') as dst:shutil.copyfileobj(src,dst)
            row=dict(family=family,input_sha256=hashlib.sha256(trace.read_bytes()).hexdigest(),runs=[])
            for rep in range(repeats):
                start=time.monotonic()
                r=subprocess.run(['/usr/bin/time','-f','%M','-o',str(rss),str(code/'nds_profile'),str(trace),str(result)],check=True,capture_output=True,text=True,timeout=a.timeout)
                wall=time.monotonic()-start
                subprocess.run([str(code/'nds_validate'),str(trace),str(result)],check=True,capture_output=True,timeout=a.timeout)
                row['runs'].append(dict(repetition=rep+1,valid=True,profile=json.loads(r.stdout),whole_player_seconds=wall,
                    max_rss_kib=int(rss.read_text().strip()),output_sha256=hashlib.sha256(result.read_bytes()).hexdigest()))
                print(family,rep+1,'passed',flush=True)
            assert len({x['output_sha256'] for x in row['runs']})==1
            report['cases'].append(row)
            (a.output/'stress_results.json').write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
