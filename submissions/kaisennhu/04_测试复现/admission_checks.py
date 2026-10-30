#!/usr/bin/env python3
"""Behavioral checks for idle traffic, replacement, and cache admission."""
import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--code', type=Path, required=True)
    parser.add_argument('--data', type=Path, default=Path(__file__).resolve().parent / 'fixtures/admission')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    code = args.code.resolve()
    rows = []
    with tempfile.TemporaryDirectory(prefix='nds-admission-') as tmp:
        target = Path(tmp) / 'result.json'
        for case in json.loads((args.data / 'manifest.json').read_text()):
            source = args.data / case['file']
            assert hashlib.sha256(source.read_bytes()).hexdigest() == case['sha256']
            subprocess.run([str(code / 'nds_player'), str(source), str(target)], check=True,
                           capture_output=True, timeout=120)
            subprocess.run([str(code / 'nds_validate'), str(source), str(target)], check=True,
                           capture_output=True, timeout=120)
            result = json.loads(target.read_text())
            metrics = result['metrics']
            name = source.stem
            if name.startswith(('cycle_', 'sparse_')) or name == 'idle_with_tail_aligned':
                assert metrics['promote_count'] == 0, (name, 'idle aligned traffic must not buy cache copies')
            if name == 'shift_k8_gap2000':
                assert any(p['source_submit_id'] >= 7 for p in result['promotion_decisions']), 'new hotspot must be admitted'
                intended = {(e['submit']['submit_id'], io['iocb_id'])
                            for e in json.loads(source.read_text())['events'] if e['submit']['submit_id'] >= 7
                            for io in e['submit']['iocbs']}
                misses = {(d['submit_id'], part['iocb_id']) for d in result['schedule_decisions']
                          for op in d['operations'] for part in op['ranges']}
                assert intended - misses, 'cache must serve the second phase as well'
            if name == 'idle_with_tail_unaligned':
                assert metrics['promote_count'] > 0 and metrics['cache_hits'] > 0, 'retain command savings in idle traffic'
            rows.append(dict(name=name, status='passed', input_sha256=case['sha256'], metrics=metrics))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(dict(source_sha256=hashlib.sha256((code / 'player/solution.cpp').read_bytes()).hexdigest(),
                                          passed=len(rows), tests=rows), indent=2) + '\n')
    print(len(rows), 'admission checks passed')

if __name__ == '__main__':
    main()
