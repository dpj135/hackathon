"""Rebuild generated artifacts without duplicating the repository's public SDK."""
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATED = ['resources','distribution','regression','cache_mix','blind','admission','holdout','engine_holdout','planning_holdout','transfer_holdout']
FIXED_FIXTURES_COMMIT = '519ef500ad5f685a7e7e010c076960610018c20b'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def sdk(code):
    manifest = json.loads((ROOT/'05_自动化测试与复现/依赖来源/SDK来源与哈希.json').read_text())
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
    tests = ROOT/'05_自动化测试与复现'
    for group in groups:
        dest = tests/'fixtures'/group
        records = json.loads((dest/'manifest.json').read_text())
        missing = [x for x in records if not (dest/x['file']).is_file() or digest(dest/x['file']) != x['sha256']]
        if not missing:
            continue
        # Keep only manifests in the submission. These fixed inputs already exist
        # in the repository's immutable history; generated families use seeds below.
        fixed = [x for x in missing if group == 'coverage' or
                 (group == 'robustness' and x['file'].startswith('length_'))]
        for item in fixed:
            source = f"{FIXED_FIXTURES_COMMIT}:submissions/kaisennhu/04_测试复现/fixtures/{group}/{item['file']}"
            result = subprocess.run(['git', '-C', str(ROOT.parents[1]), 'show', source],
                                    capture_output=True)
            if result.returncode:
                raise RuntimeError('固定输入需完整Git历史；请在仓库执行 git fetch origin '+
                                   FIXED_FIXTURES_COMMIT+' 后重试：'+group+'/'+item['file'])
            if hashlib.sha256(result.stdout).hexdigest() != item['sha256']:
                raise RuntimeError('固定输入哈希不符：'+group+'/'+item['file'])
            (dest/item['file']).write_bytes(result.stdout)
        missing = [x for x in missing if x not in fixed]
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
