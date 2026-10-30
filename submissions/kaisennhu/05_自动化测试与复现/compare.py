#!/usr/bin/env python3
"""Assessment only: compare fixed current policy with organizer's greedy."""
import re
import argparse,hashlib,json,math,os,subprocess,tempfile,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--code',type=Path,required=True);p.add_argument('--data',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--repeat',type=int,default=3);p.add_argument('--timeout',type=float,default=600);a=p.parse_args()
code=a.code.resolve();build=code
os.sched_setaffinity(0,{max(os.sched_getaffinity(0))})
manifest=json.loads((a.data/'manifest.json').read_text())
report=dict(source_sha256=hashlib.sha256((code/'player/solution.cpp').read_bytes()).hexdigest(),repeat=a.repeat,auxiliary_timeout_seconds=a.timeout,cases=[],summary={})
report['binary_sha256']=hashlib.sha256((code/'nds_player').read_bytes()).hexdigest()
report['baseline_binary_sha256']=hashlib.sha256((code/'nds_greedy_metered').read_bytes()).hexdigest()
report['engine_files_sha256']={str(p.relative_to(code)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [code/'src/framework.cpp',code/'src/player_main.cpp',code/'scripts/score_suite.py']}
a.output.parent.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='nds-review-') as tmp:
    dest=Path(tmp)/'result.json'
    for case in manifest:
        src=a.data/case['file'];digest=hashlib.sha256(src.read_bytes()).hexdigest();assert digest==case['sha256']
        row=dict(file=src.name,family=case['family'],input_sha256=digest,variants={})
        for name in ['strategy','baseline']:
            runs=[]
            for rep in range(a.repeat):
                run=dict(repetition=rep+1);t=time.monotonic()
                try:
                    played=subprocess.run([str(build/('nds_player' if name=='strategy' else 'nds_greedy_metered')),str(src),str(dest)],check=True,capture_output=True,timeout=a.timeout)
                    peak=re.search(rb'^NDS_POLICY_PEAK_KB (\d+)$',played.stderr,re.M)
                    if not peak:raise RuntimeError('player did not report policy memory')
                    run['policy_peak_kib']=int(peak[1])
                    run['seconds']=time.monotonic()-t
                    subprocess.run([str(build/'nds_validate'),str(src),str(dest)],check=True,capture_output=True,timeout=a.timeout)
                    raw=dest.read_bytes();run.update(valid=True,output_sha256=hashlib.sha256(raw).hexdigest(),metrics=json.loads(raw)['metrics'])
                except (subprocess.CalledProcessError,subprocess.TimeoutExpired) as exc:
                    run.update(valid=False,error=str(exc),stderr=(exc.stderr or b'').decode(errors='replace')[-1500:])
                runs.append(run)
            row['variants'][name]=dict(valid=all(r['valid'] for r in runs),runs=runs)
            if row['variants'][name]['valid']:assert len({r['output_sha256'] for r in runs})==1
        report['cases'].append(row);a.output.write_text(json.dumps(report,indent=2)+'\n')
        print(src.stem,{n:v['runs'][0].get('metrics',{}).get('total_cost','FAIL') for n,v in row['variants'].items()},flush=True)
for family in ['all']+sorted({c['family'] for c in report['cases']}):
    rows=[c for c in report['cases'] if family=='all' or c['family']==family]
    pairs=[(c['variants']['strategy']['runs'][0]['metrics']['total_cost'],c['variants']['baseline']['runs'][0]['metrics']['total_cost']) for c in rows if all(v['valid'] for v in c['variants'].values())]
    ratios=[s/b for s,b in pairs]
    report['summary'][family]=dict(cases=len(rows),valid_pairs=len(pairs),wins=sum(s<b for s,b in pairs),ties=sum(s==b for s,b in pairs),losses=sum(s>b for s,b in pairs),geometric_ratio=math.exp(sum(map(math.log,ratios))/len(ratios)) if ratios else None,worst_ratio=max(ratios,default=None))
a.output.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report['summary'],indent=2))
if any(not v['valid'] for c in report['cases'] for v in c['variants'].values()):
    raise SystemExit(1)
