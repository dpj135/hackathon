#!/usr/bin/env python3
"""Offline live demo; authoritative replay plus cost comparison, no external assets."""
import argparse,json,subprocess,collections
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--code',type=Path);p.add_argument('--output',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[1];code=(a.code or root/'03_程序').resolve();out=(a.output or root/'06_答辩演示/运行输出').resolve();out.mkdir(parents=True,exist_ok=True)
subprocess.run(['make','-C',str(code),'-j4'],check=True,stdout=subprocess.DEVNULL)
scenes=[('read_direct_low','连续512B读取：比较命令合并'),('promote_low_aligned','热点复用：观察PROMOTE和CACHE_HIT'),('promote_high','小缓存大工作集：限制提升成本'),('promote_ssd_mid','边际资源工作量估值：拒绝小对象低收益提升')]
scenes.append(('short_ttl_26102901','扩展负载：排队后热点证据过时'))
scenes.extend([('shift_k8_gap2000','热点迁移：允许新数据替换旧缓存'),
               ('idle_with_tail_unaligned','稀疏非对齐读取：保留调用成本收益')])
for name,title in scenes:
 src=code/f'data/{name}.json'
 if not src.exists():
  src=next(p for p in [root/f'04_测试复现/fixtures/{group}/{name}.json' for group in ['resources','admission']] if p.exists())
 print('\n'+title,flush=True);solutions={}
 for label,exe in [('贪心','nds_baseline'),('方案','nds_player')]:
  path=out/f'{name}_{exe}.json'
  subprocess.run([str(code/exe),str(src),str(path)],check=True,stdout=subprocess.DEVNULL)
  check=subprocess.run([str(code/'nds_validate'),str(src),str(path)],check=True,capture_output=True,text=True)
  assert json.loads(check.stdout)['valid']
  r=json.loads(path.read_text());m=r['metrics'];solutions[label]=m
  paths=collections.Counter(o['path'] for d in r['schedule_decisions'] for o in d['operations'])
  print(f"  {label}：J={m['total_cost']}，前台操作={sum(paths.values())}，提升={m['promote_count']}，缓存命中={m['cache_hits']}，驱逐={m['evictions']}，回放合法")
  print('  路径：'+str(dict(paths)))
 gain=100*(1-solutions['方案']['total_cost']/solutions['贪心']['total_cost'])
 print(f'  方案相对官方贪心：成本降低 {gain:.2f}%')
print('\n真实决策、完成事件和指标已保存到：'+str(out))
