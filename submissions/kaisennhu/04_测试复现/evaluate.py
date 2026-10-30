#!/usr/bin/env python3
"""Run the submitted policy, replay every output, retain deterministic evidence."""
import re
import argparse
import hashlib
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--code',type=Path,required=True)
    p.add_argument('--data',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--repeat',type=int,default=3)
    p.add_argument('--timeout',type=float,default=180)
    p.add_argument('--cpu',type=int)
    a=p.parse_args();code=a.code.resolve();data=a.data.resolve()
    if a.repeat<1:p.error('--repeat must be positive')
    if a.cpu is not None:os.sched_setaffinity(0,{a.cpu})
    manifest=data/'manifest.json'
    cases=json.loads(manifest.read_text()) if manifest.exists() else [dict(file=f.name) for f in sorted(data.glob('*.json'))]
    if not cases:p.error('no input cases')
    result=dict(source_sha256=sha(code/'player/solution.cpp'),binary_sha256=sha(code/'nds_player'),
                repeat=a.repeat,cpu_affinity=sorted(os.sched_getaffinity(0)),cases=[],summary={})
    result['engine_files_sha256']={str(p.relative_to(code)):sha(p) for p in [code/'src/framework.cpp',code/'src/player_main.cpp',code/'scripts/score_suite.py']}
    a.output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='nds-evaluate-') as tmp:
        dest=Path(tmp)/'result.json'
        for case in cases:
            src=data/case['file'];digest=sha(src)
            if 'sha256' in case and case['sha256']!=digest:raise RuntimeError(f'changed input: {src}')
            row=dict(file=src.name,family=case.get('family'),input_sha256=digest,runs=[])
            for rep in range(a.repeat):
                run=dict(repetition=rep+1);start=time.monotonic()
                try:
                    played=subprocess.run([str(code/'nds_player'),str(src),str(dest)],check=True,capture_output=True,timeout=a.timeout)
                    peak=re.search(rb'^NDS_POLICY_PEAK_KB (\d+)$',played.stderr,re.M)
                    if not peak:raise RuntimeError('player did not report policy memory')
                    run['policy_peak_kib']=int(peak[1])
                    run['player_seconds']=time.monotonic()-start
                    subprocess.run([str(code/'nds_validate'),str(src),str(dest)],check=True,capture_output=True,timeout=a.timeout)
                    run.update(valid=True,output_sha256=sha(dest),metrics=json.loads(dest.read_text())['metrics'])
                except (subprocess.TimeoutExpired,subprocess.CalledProcessError) as exc:
                    run.update(valid=False,error=str(exc),stderr=(exc.stderr or b'').decode(errors='replace')[-2000:])
                row['runs'].append(run)
            row['valid']=all(x['valid'] for x in row['runs'])
            row['deterministic']=row['valid'] and len({x['output_sha256'] for x in row['runs']})==1
            result['cases'].append(row)
            a.output.write_text(json.dumps(result,indent=2)+'\n')
            print(src.stem,row['runs'][0].get('metrics',{}).get('total_cost','FAIL'),flush=True)
    result['summary']=dict(cases=len(cases),valid=sum(c['valid'] for c in result['cases']),
                           deterministic=sum(c['deterministic'] for c in result['cases']),player_runs=len(cases)*a.repeat)
    a.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result['summary']))
    return int(any(not c['valid'] or not c['deterministic'] for c in result['cases']))

if __name__=='__main__':raise SystemExit(main())
