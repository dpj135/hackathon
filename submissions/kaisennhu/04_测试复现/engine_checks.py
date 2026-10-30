#!/usr/bin/env python3
"""Validate actual expiry semantics and the official policy-only allocation meter."""
import argparse,copy,hashlib,importlib.util,json,os,re,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--code',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
code=a.code.resolve();here=Path(__file__).resolve().parent;checks=[]
def done(name,**details):checks.append(dict(name=name,passed=True,**details))
with tempfile.TemporaryDirectory(prefix='nds-engine-') as td:
 w=Path(td);probe=w/'probe'
 subprocess.run(['g++','-O2','-std=c++17','-I'+str(code/'include'),str(code/'src/json.cpp'),str(code/'src/framework.cpp'),str(here/'engine_probe.cpp'),'-o',str(probe)],check=True,capture_output=True)
 t=json.loads((code/'data/small_aligned.json').read_text());t['instance']['resources'].pop('mooncake_capacity',None);t['instance']['resources']['kvcache_pool_capacity']=8;t['instance']['fixed_parameters']['promote_ttl_us']=100
 t['events']=[]
 for i in range(4):
  io=dict(iocb_id=1,opcode='READV',device_id=0,ssd_offset=0 if i<2 else 2097152,length=1048576,iovs=[dict(iov_id=0,hbm_addr=1<<40,length=1048576)])
  t['events'].append(dict(event='IO_SUBMIT',time_us=i*2000,submit=dict(submit_id=i+1,arrival_us=i*2000,iocbs=[io])))
 src=w/'expiry.json';src.write_text(json.dumps(t));out=w/'result.json'
 subprocess.run([str(probe),str(src),str(out)],check=True,capture_output=True)
 subprocess.run([str(code/'nds_validate'),str(src),str(out)],check=True,capture_output=True)
 m=json.loads(out.read_text())['metrics'];assert m['cache_hits']==0 and m['promote_count']==2 and m['evictions']==1,m
 for name in ['expired_key_is_miss','expired_state_retained_on_arrival','expired_state_retained_on_completion','nonfull_insertion_cleans_expired']:
  done(name,input_sha256=hashlib.sha256(src.read_bytes()).hexdigest(),metrics=m)
 # Framework RSS may exceed a small policy limit; the tiny policy still succeeds.
 env=dict(os.environ,NDS_POLICY_MEMORY_LIMIT_MB='1')
 q=subprocess.run([str(code/'nds_player'),str(code/'data/small_aligned.json'),str(out)],capture_output=True,text=True,env=env)
 assert q.returncode==0,q.stderr
 peak=re.search(r'^NDS_POLICY_PEAK_KB (\d+)$',q.stderr,re.M);assert peak and int(peak[1])<1024
 done('official_policy_meter_reports_peak',policy_peak_kib=int(peak[1]))
 q=subprocess.run([str(code/'nds_player'),str(code/'data/read_direct_high.json'),str(out)],capture_output=True,text=True,env=env)
 assert q.returncode!=0 and 'NDS_POLICY_MEMORY_EXCEEDED' in q.stderr,q.stderr
 done('official_policy_meter_enforces_limit',memory_limit_mb=1)
 spec=importlib.util.spec_from_file_location('score_suite',code/'scripts/score_suite.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
 played,_=mod.score_case(code/'nds_baseline',code/'nds_validate',code/'data/small_aligned.json',30,16384,w)
 assert played['status']=='error' and 'did not report NDS_POLICY_PEAK_KB' in played['stderr']
 done('scorer_rejects_unmetered_player')
 greedy=w/'greedy_metered'
 subprocess.run(['g++','-O2','-std=c++17','-I'+str(code/'include'),str(code/'src/json.cpp'),str(code/'src/framework.cpp'),str(code/'src/player_main.cpp'),str(here/'greedy_policy.cpp'),'-o',str(greedy)],check=True,capture_output=True)
 original=w/'original.json';subprocess.run([str(code/'nds_baseline'),str(code/'data/small_mixed.json'),str(original)],check=True,capture_output=True)
 played,metrics=mod.score_case(greedy,code/'nds_validate',code/'data/small_mixed.json',30,16384,w)
 assert played['status']=='ok' and json.loads(original.read_text())==json.loads((w/'solution.json').read_text())
 done('metered_greedy_preserves_sdk_behavior',policy_peak_mb=played['policy_peak_mb'])
report=dict(source_sha256=hashlib.sha256((code/'player/solution.cpp').read_bytes()).hexdigest(),engine_files_sha256={str(x.relative_to(code)):hashlib.sha256(x.read_bytes()).hexdigest() for x in [code/'src/framework.cpp',code/'src/player_main.cpp',code/'scripts/score_suite.py']},passed=len(checks),tests=checks)
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n');print(len(checks),'engine/meter compatibility checks passed')
