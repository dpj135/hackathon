#!/usr/bin/env python3
"""Verify file hashes, source/binaries, PPTX and four official replay examples."""
import argparse
import hashlib
import json
import platform
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--files-only', action='store_true',
                        help='Verify files without executing Linux binaries')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    required = ['01_调度算法源代码/player/solution.cpp',
                '02_可执行程序/linux-x86_64/nds_player',
                '02_可执行程序/linux-x86_64/nds_validate',
                '03_输入输出说明/输入输出格式.md',
                '04_算法设计与复杂度/算法设计.md',
                '05_自动化测试与复现/测试操作手册.md',
                '06_性能与资源报告/验证报告.md',
                '06_性能与资源报告/资源开销.csv',
                '07_答辩PPT/答辩介绍.pptx', 'run.py']
    records = {}
    for line in (root/'文件校验SHA256.txt').read_text().splitlines():
        sha, relative = line.split('  ', 1)
        target = (root/relative).resolve()
        if not target.is_relative_to(root) or relative in records:
            raise RuntimeError('Invalid manifest path: '+relative)
        if not target.is_file() or digest(target) != sha:
            raise RuntimeError('Missing or modified file: '+relative)
        records[relative] = sha
    if not set(required).issubset(records):
        raise RuntimeError('Manifest does not cover all required deliverables')
    binaries = root/'02_可执行程序/linux-x86_64'
    manifest = json.loads((binaries/'manifest.json').read_text())
    if digest(root/required[0]) != manifest['source_sha256']:
        raise RuntimeError('Source differs from the binary build manifest')
    for name, sha in manifest['binaries'].items():
        if digest(binaries/name) != sha:
            raise RuntimeError('Binary hash mismatch: '+name)
    ppt = root/'07_答辩PPT/答辩介绍.pptx'
    with zipfile.ZipFile(ppt) as archive:
        bad = archive.testzip()
        if bad:
            raise RuntimeError('Corrupt PPTX member: '+bad)
        presentation = ET.fromstring(archive.read('ppt/presentation.xml'))
        ns = {'p':'http://schemas.openxmlformats.org/presentationml/2006/main'}
        slides = presentation.findall('p:sldIdLst/p:sldId', ns)
        if len(slides) != 12:
            raise RuntimeError(f'Expected 12 slides, found {len(slides)}')
    print(f'PASS {len(records)} file hashes; source/binaries match; 12-slide PPTX')
    if args.files_only:
        return
    if platform.system() != 'Linux' or platform.machine() not in ['x86_64', 'AMD64']:
        parser.error('Replay requires Linux x86_64; use --files-only or build from source')
    import prepare
    code = root/'01_调度算法源代码'
    prepare.sdk(code)
    for name, sha in manifest['engine_files_sha256'].items():
        if digest(code/name) != sha:
            raise RuntimeError('SDK hash mismatch: '+name)
    for name in manifest['binaries']:
        p = binaries/name
        p.chmod(p.stat().st_mode | 0o100)
    examples = root/'04_算法设计与复杂度/示例'
    with tempfile.TemporaryDirectory(prefix='nds-delivery-') as tmp:
        for src in sorted(examples.glob('*输入.json')):
            result = Path(tmp)/src.name
            subprocess.run([binaries/'nds_player', src, result], check=True,
                           capture_output=True, timeout=60)
            subprocess.run([binaries/'nds_validate', src, result], check=True,
                           capture_output=True, timeout=60)
            reference = examples/src.name.replace('输入', '输出')
            if digest(result) != digest(reference):
                raise RuntimeError('Example output changed: '+src.name)
            print('PASS replay '+src.name)


if __name__ == '__main__':
    main()
