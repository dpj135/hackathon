#!/usr/bin/env python3
"""Check that the strategy object uses data helpers, not the engine or file APIs."""
import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--code', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
code = a.code.resolve()
with tempfile.TemporaryDirectory(prefix='nds-policy-boundary-') as temp:
    obj = Path(temp) / 'solution.o'
    subprocess.run(['g++', '-O2', '-std=c++17', '-I' + str(code / 'include'),
                    '-c', str(code / 'player/solution.cpp'), '-o', str(obj)], check=True)
    symbols = subprocess.check_output(['nm', '-C', '-u', str(obj)], text=True)
    sdk = [line.strip().removeprefix('U ') for line in symbols.splitlines() if 'nds::' in line]
    allowed = ('nds::Json::', 'nds::align_down(', 'nds::align_up(', 'nds::physical_span(')
    assert sdk and all(symbol.startswith(allowed) for symbol in sdk), sdk
    # This is a build dependency guard, not a general sandbox/security proof.
    forbidden = ('Simulator', 'load_trace', 'read_json_file', 'write_json_file',
                 'fopen', 'ifstream', 'ofstream', 'socket', 'connect', 'system(', 'execve')
    assert not any(name in symbols for name in forbidden), symbols
report = dict(source_sha256=hashlib.sha256((code / 'player/solution.cpp').read_bytes()).hexdigest(),
              passed=1, sdk_dependencies=sdk)
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(report, indent=2) + '\n')
print('Strategy object has no engine or trace/file API dependency')
