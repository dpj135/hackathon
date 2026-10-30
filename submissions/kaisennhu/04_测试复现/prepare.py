"""Rebuild generated artifacts without duplicating the repository's public SDK."""
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATED = ['resources','distribution','regression','cache_mix','blind','admission','holdout','engine_holdout','planning_holdout']

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def sdk(code):
    manifest = json.loads((ROOT/'01_题目与依据/SDK来源与哈希.json').read_text())
    official = ROOT.parents[1]/'public'
    for item in manifest['files']:
        dst = code/item['file']
        if dst.is_file() and digest(dst) == item['sha256']:
            continue
        src = official/item['file']
        if not src.is_file() or digest(src) != item['sha256']:
            raise RuntimeError('SDK 缺失或版本不符，请使用完整仓库；固定版本：'+manifest['commit']+'；文件：'+item['file'])
        dst.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(src,dst)

def fixtures(code, groups):
    tests = ROOT/'04_测试复现'
    for group in groups:
        dest = tests/'fixtures'/group
        records = json.loads((dest/'manifest.json').read_text())
        missing = [x for x in records if not (dest/x['file']).is_file() or digest(dest/x['file']) != x['sha256']]
        if not missing:
            continue
        if group not in GENERATED and group != 'robustness':
            raise RuntimeError('固定输入缺失或哈希不符：'+group)
        with tempfile.TemporaryDirectory(prefix='nds-fixtures-') as tmp:
            out = Path(tmp)
            args = ['--code',str(code),'--output',str(out)]
            if group in ['resources','distribution','regression']:
                import matrix
                matrix.generate(code,out,'dev',sorted({x['seed'] for x in records}))
            else:
                script = {'cache_mix':'independent.py','blind':'renewal.py','admission':'admission_workloads.py','robustness':'robustness_inputs.py'}.get(group,'admission_holdout.py')
                if script == 'admission_holdout.py':
                    args += ['--source-sha256',digest(code/'player/solution.cpp'),'--seeds',*map(str,sorted({x['seed'] for x in records}))]
                subprocess.run(['python3',str(tests/script),*args],check=True)
            for item in missing:
                src = out/item['file']
                if digest(src) != item['sha256']:
                    raise RuntimeError('重新生成的输入与冻结哈希不同：'+group+'/'+item['file'])
                shutil.copy2(src,dest/item['file'])
