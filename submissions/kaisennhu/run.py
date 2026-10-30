#!/usr/bin/env python3
"""One entry point for building, verifying and demonstrating the submitted policy."""
import argparse
import hashlib
import json
import math
import os
import platform
import subprocess
import sys
from datetime import datetime,timezone
from pathlib import Path

def require_valid_score(score):
    cases=score['cases'];count=score['case_count']
    if not cases or count!=len(cases) or score['valid_cases']!=count:
        raise ValueError('评分未全部通过：合法条数不完整')
    if len({c['id'] for c in cases})!=count:
        raise ValueError('评分包含重复用例')
    if any(c['status']!='ok' or c.get('total_cost',0)<=0 or
           not isinstance(c.get('policy_peak_mb'),(int,float)) or
           not math.isfinite(c['policy_peak_mb']) or c['policy_peak_mb']<0 or
           (score['memory_limit_mb']>0 and c['policy_peak_mb']>score['memory_limit_mb']) for c in cases):
        raise ValueError('评分存在失败、无效成本或内存超限')
    if score['performance']!=10.0 or score['feasibility']!=20.0:
        raise ValueError('评分资源或正确性检查未全部通过')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode',choices=['build','solve','quick','score','extended','compare','scale','demo','all'],nargs='?',default='quick')
    p.add_argument('--data',type=Path,help='Additional data folder for extended tests')
    p.add_argument('--repeat',type=int,default=3)
    p.add_argument('--output',type=Path)
    p.add_argument('--input',type=Path,help='Trace JSON for solve')
    p.add_argument('--result',type=Path,help='Solution JSON for solve')
    p.add_argument('--prebuilt',action='store_true',help='Use the verified Linux x86_64 executables for solve')
    a=p.parse_args()
    if a.repeat < 1:p.error('--repeat must be positive')
    root=Path(__file__).resolve().parent;code=root/'03_程序';test=root/'04_测试复现'
    if os.name!='posix':p.error('Run build/tests under Linux or Windows WSL; documents can be opened directly on Windows.')
    if a.mode=='solve' and a.input is None:p.error('solve requires --input')
    if a.prebuilt and a.mode!='solve':p.error('--prebuilt is available for solve')
    out=(a.output or root/'运行结果'/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')).resolve();out.mkdir(parents=True,exist_ok=True)
    def run(cmd,log=None):
        args=list(map(str,cmd))
        if log:
            with (out/log).open('w') as f:subprocess.run(args,check=True,stdout=f,stderr=subprocess.STDOUT)
        else:subprocess.run(args,check=True)
    sys.path.insert(0,str(test))
    import prepare
    prepare.sdk(code)
    binaries=code
    if a.prebuilt:
        if platform.system()!='Linux' or platform.machine() not in ['x86_64','AMD64']:
            p.error('The bundled executables require Linux x86_64; build from source on other supported Linux architectures.')
        binaries=code/'bin/linux-x86_64'
        if not (binaries/'manifest.json').is_file():p.error('Git仓库只提供源码；请去掉--prebuilt自动构建，或使用完整本地交付。')
        manifest=json.loads((binaries/'manifest.json').read_text())
        if hashlib.sha256((code/'player/solution.cpp').read_bytes()).hexdigest()!=manifest['source_sha256']:
            raise RuntimeError('源码与可执行程序不匹配，请重新构建')
        for name,digest in manifest['engine_files_sha256'].items():
            if hashlib.sha256((code/name).read_bytes()).hexdigest()!=digest:
                raise RuntimeError('SDK与可执行程序不匹配：'+name)
        for name,digest in manifest['binaries'].items():
            target=binaries/name
            if hashlib.sha256(target.read_bytes()).hexdigest()!=digest:raise RuntimeError('可执行程序校验失败：'+name)
            target.chmod(target.stat().st_mode|0o100)
    else:
        print('正在构建程序……',flush=True)
        run(['make','-C',code,'-j4'], 'build.log')
        print('构建完成。',flush=True)
    if a.mode=='solve':
        result=(a.result or out/'solution.json').resolve();result.parent.mkdir(parents=True,exist_ok=True)
        run([binaries/'nds_player',a.input.resolve(),result])
        run([binaries/'nds_validate',a.input.resolve(),result])
        print('方案与官方回放校验完成：',result)
    if a.mode in ['quick','all']:
        prepare.fixtures(code,['admission'])
        run(['make','-C',code,'test'],'framework.log')
        run(['python3',test/'regression.py','--code',code,'--output',out/'regression'])
        run(['python3',test/'boundaries.py','--code',code,'--output',out/'boundaries'])
        run(['python3',test/'admission_checks.py','--code',code,'--output',out/'admission_checks.json'])
        run(['python3',test/'gate_tests.py','--output',out/'gate_checks.json'])
        run(['python3',test/'engine_checks.py','--code',code,'--output',out/'engine_checks.json'])
    if a.mode in ['score','compare','all']:
        run(['g++','-O2','-std=c++17','-I'+str(code/'include'),code/'src/json.cpp',code/'src/framework.cpp',code/'src/player_main.cpp',test/'greedy_policy.cpp','-o',code/'nds_greedy_metered'])
    if a.mode in ['score','all']:
        for label,binary in [('strategy','nds_player'),('official_baseline','nds_greedy_metered')]:
            print('正在评测：'+('提交方案' if label=='strategy' else '官方基线'),flush=True)
            run(['python3',code/'scripts/score_suite.py','--player',code/binary,'--validate',code/'nds_validate',
                '--suite',code/'suites/public_suite.json','--data-dir',code/'data','--baseline',code/'suites/public_baseline.json',
                '--frozen',code/'suites/frozen_costs.json','--label',label,'--output',out/(label+'_score.json')],label+'_score.log')
            score=json.loads((out/(label+'_score.json')).read_text())
            require_valid_score(score)
            print(f"  自动分 {score['automated']}/90，合法 {score['valid_cases']}/{score['case_count']}",flush=True)
    if a.mode in ['extended','compare','all'] and not a.data:
        prepare.fixtures(code,prepare.GENERATED+['robustness','coverage'])
    if a.mode=='demo':
        prepare.fixtures(code,['resources','admission'])
    if a.mode in ['extended','all']:
        groups=[('additional',a.data.resolve())] if a.data else [('official',code/'data')]+[(n,test/'fixtures'/n) for n in ['resources','coverage','distribution','regression','robustness','cache_mix','blind','admission','holdout','engine_holdout','planning_holdout']]
        for name,data in groups:
            print(f'正在验证 {name}，每个输入重复 {a.repeat} 次……',flush=True)
            run(['python3',test/'evaluate.py','--code',code,'--data',data,'--repeat',a.repeat,'--cpu',min(os.sched_getaffinity(0)),
                 '--output',out/(name+'.json')],name+'.log')
            summary=json.loads((out/(name+'.json')).read_text())['summary']
            print(f"  {summary['valid']}/{summary['cases']} 合法，{summary['deterministic']} 条重复输出一致。",flush=True)
    if a.mode in ['compare','all']:
        groups=[('additional',a.data.resolve())] if a.data else [(n,test/'fixtures'/n) for n in ['regression','robustness','cache_mix','blind','admission','holdout','engine_holdout','planning_holdout']]
        for name,data in groups:
            print(f'正在比较提交策略与官方贪心：{name}',flush=True)
            run(['python3',test/'compare.py','--code',code,'--data',data,'--repeat',a.repeat,
                 '--output',out/(name+'_comparison.json')],name+'_comparison.log')
    if a.mode in ['scale','all']:
        run(['g++','-O2','-std=c++17','-I'+str(code/'include'),code/'src/json.cpp',code/'src/framework.cpp',code/'player/solution.cpp',test/'profile_main.cpp','-o',code/'nds_profile'])
        run(['python3',test/'stress.py','--code',code,'--output',out/'stress'])
    if a.mode in ['demo','all']:
        run(['python3',root/'06_答辩演示/demo.py','--output',out/'demo'])
    print('结果目录：',out)

if __name__=='__main__':main()
