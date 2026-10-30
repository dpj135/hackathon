#!/usr/bin/env python3
"""Check the frozen 655-case dataset without building or running a policy."""
import hashlib
import json
from collections import Counter
from pathlib import Path


EXPECTED_GROUPS = {
    'official': 39, 'resources': 72, 'coverage': 54, 'distribution': 96,
    'regression': 96, 'robustness': 33, 'cache_mix': 24, 'blind': 24,
    'admission': 19, 'holdout': 24, 'engine_holdout': 24,
    'planning_holdout': 24, 'transfer_holdout': 24,
    'pr17_added': 78, 'ablation_holdout': 24,
}


def main():
    root = Path(__file__).resolve().parent
    cases = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
    if Counter(case['group'] for case in cases) != EXPECTED_GROUPS:
        raise ValueError('Dataset groups or counts differ from the frozen 655 cases')
    paths, hashes, ids = set(), set(), set()
    total_bytes = 0
    for case in cases:
        path = (root / case['file']).resolve()
        expected_parent = (root / '../../public/data' if case['group'] == 'official'
                           else root / 'inputs' / case['group']).resolve()
        if path.parent != expected_parent or path.suffix != '.json':
            raise ValueError('Unexpected input path: ' + case['file'])
        if path in paths or case['id'] in ids:
            raise ValueError('Duplicate path or ID: ' + case['file'])
        payload = path.read_bytes()
        digest = hashlib.sha256(payload).hexdigest()
        if len(payload) != case['bytes'] or digest != case['sha256']:
            raise ValueError('Missing or modified input: ' + case['file'])
        if digest in hashes:
            raise ValueError('Byte-identical duplicate input: ' + case['file'])
        if not isinstance(json.loads(payload), dict):
            raise ValueError('Input must be a JSON object: ' + case['file'])
        paths.add(path)
        hashes.add(digest)
        ids.add(case['id'])
        total_bytes += len(payload)
    actual = {p.resolve() for p in (root / 'inputs').rglob('*.json')}
    expected = {p for p in paths if p.is_relative_to(root / 'inputs')}
    if actual != expected:
        raise ValueError('Unlisted or missing supplemental input files')
    print(f'PASS: {len(cases)} cases, 15 groups, unique SHA256 values; '
          f'{total_bytes:,} input bytes; all JSON objects parsed')
    print('Official: 39 shared inputs; supplemental: 616 inputs')


if __name__ == '__main__':
    main()
