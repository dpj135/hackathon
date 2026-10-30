#!/usr/bin/env python3
"""Compare foreground/cache metrics and cache-on/off J on the fixed public suite."""
import argparse
import csv
import hashlib
import json
import subprocess
import tempfile
from collections import Counter
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args):
    return subprocess.run(list(map(str, args)), check=True, capture_output=True, timeout=300)


def metrics(trace, result):
    requests = {(e['submit']['submit_id'], io['iocb_id']): (io, e['submit']['arrival_us'])
                for e in trace['events'] if e['event'] == 'IO_SUBMIT'
                for io in e['submit']['iocbs']}
    costs = trace['instance']['fixed_parameters']
    owners = set()
    counts = Counter(requests=len(requests), J=result['metrics']['total_cost'])

    def account(path, transfers, scope):
        types = Counter(t['type'] for t in transfers)
        ssd = sum(v for k, v in types.items() if k == 'DIRECT' or k.startswith('SSD_TO_'))
        prefix = 'CPU_NDS' if path in ('DIRECT', 'AGGREGATE') else 'CPU_FALLBACK' if path == 'FALLBACK' else 'CPU_PROMOTE'
        fee = costs[prefix+'_FIXED_COST'] + ssd*costs[prefix+'_PER_CMD_COST']
        for kind in ('H2D', 'D2H', 'D2D'):
            fee += types[kind]*(costs['CPU_'+kind+'_CALL_COST']+costs['NPU_'+kind+'_CALL_COST'])
        counts[scope+'_calls'] += fee
        counts[scope+'_ssd_commands'] += ssd
        for t in transfers:
            counts[scope+'_bytes'] += t.get('physical_length', t.get('bytes', 0))
            if t['type'] == 'DIRECT' or t['type'].startswith('SSD_TO_'):
                counts[scope+'_ssd_bytes'] += t['physical_length']

    for decision in result['schedule_decisions']:
        for op in decision['operations']:
            counts['fg_operations'] += 1
            counts['path_'+op['path']] += 1
            owners.update((decision['submit_id'], r['iocb_id']) for r in op['ranges'])
            account(op['path'], op['transfers'], 'fg')
    for decision in result['promotion_decisions']:
        counts['promotions'] += 1
        account(decision['path'], [decision['transfer']], 'promote')
    hit_owners = set(requests)-owners
    counts['hits'] = len(hit_owners)
    counts['hit_bytes'] = sum(requests[key][0]['length'] for key in hit_owners)
    counts['hit_calls'] = counts['hits']*(costs['CPU_H2D_CALL_COST']+costs['NPU_H2D_CALL_COST'])
    for event in result['events']:
        if event['event'] == 'IOCB_COMPLETE':
            for completion in event['completions']:
                key = completion['submit_id'], completion['iocb_id']
                counts['latency_sum_us'] += completion['complete_us']-requests[key][1]
    actual = result['metrics']
    assert counts['hits'] == actual['cache_hits']
    assert counts['promotions'] == actual['promote_count']
    assert counts['fg_ssd_bytes']+counts['promote_ssd_bytes'] == actual['physical_ssd_bytes']
    assert counts['fg_calls']+counts['promote_calls']+counts['hit_calls'] == actual['cpu_cost']+actual['npu_cost']
    counts['insertions'] = actual['promote_success']
    counts['trace_us'] = actual['trace_time_us']
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[1]
    parser.add_argument('--code', type=Path, default=root/'01_调度算法源代码')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    import prepare
    code = args.code.resolve()
    prepare.sdk(code)
    args.output.mkdir(parents=True, exist_ok=True)
    source = (code/'player/solution.cpp').read_text()
    # Clean up completed Work/arrivals exactly as normal, then disable admission.
    marker = 'if (arrival != arrivals_.end()) arrivals_.erase(arrival);'
    assert source.count(marker) == 1, 'Review the cache-off insertion point for this source revision'
    off_source = source.replace(marker, marker+'\n        return std::nullopt; // experiment: disable PROMOTE')
    baseline = '#include "nds/player_api.hpp"\nstd::unique_ptr<nds::SchedulerPolicy> nds::create_policy(const std::map<std::string,std::int64_t>& r,const std::map<std::string,std::int64_t>&) { return std::make_unique<nds::GreedyBaseline>(r, ENABLE); }\n'
    suite = json.loads((code/'suites/public_suite.json').read_text())['cases']
    rows = []
    with tempfile.TemporaryDirectory(prefix='nds-cache-analysis-') as tmp:
        tmp = Path(tmp)
        objects = []
        for name in ('json', 'framework', 'player_main'):
            obj = tmp/(name+'.o'); objects.append(obj)
            run(['g++', '-O2', '-std=c++17', '-I'+str(code/'include'), '-c', code/'src'/(name+'.cpp'), '-o', obj])
        sources = {'greedy': baseline.replace('ENABLE', 'true'), 'greedy_off': baseline.replace('ENABLE', 'false'),
                   'strategy': source, 'strategy_off': off_source}
        for name, content in sources.items():
            src = tmp/(name+'.cpp'); src.write_text(content)
            run(['g++', '-O2', '-std=c++17', '-I'+str(code/'include'), src, *objects, '-o', tmp/name])
        validator = tmp/'validate'
        run(['g++', '-O2', '-std=c++17', '-I'+str(code/'include'), code/'src/validate_main.cpp', *objects[:2], '-o', validator])
        for case in suite:
            src = code/'data'/case['file']; trace = json.loads(src.read_text())
            for variant in ('greedy', 'strategy'):
                pair = {}
                for suffix in ('', '_off'):
                    output = tmp/'result.json'
                    run([tmp/(variant+suffix), src, output])
                    run([validator, src, output])
                    result = json.loads(output.read_text())
                    pair[suffix] = metrics(trace, result)
                    pair[suffix]['output_sha256'] = sha(output)
                    if suffix: assert pair[suffix]['promotions'] == pair[suffix]['hits'] == 0
                row = dict(case=case['id'], variant=variant, input_sha256=sha(src), **pair[''])
                row.update(J_without_promote=pair['_off']['J'], cache_net_J=pair['_off']['J']-pair['']['J'],
                           no_promote_output_sha256=pair['_off']['output_sha256'])
                rows.append(row)
            print(case['id'], {r['variant']: r['cache_net_J'] for r in rows[-2:]}, flush=True)
    columns = ['case','variant','input_sha256','output_sha256','no_promote_output_sha256','requests','J','J_without_promote',
               'cache_net_J','trace_us','fg_operations','fg_ssd_commands','fg_ssd_bytes','fg_calls','path_DIRECT',
               'path_AGGREGATE','path_FALLBACK','latency_sum_us','promotions','insertions','promote_bytes',
               'promote_ssd_bytes','promote_calls','hits','hit_bytes','hit_calls']
    with (args.output/'前后台指标.csv').open('w', encoding='utf-8-sig', newline='') as f:
        writer = csv.DictWriter(f, columns, extrasaction='ignore', restval=0, lineterminator='\n');writer.writeheader();writer.writerows(rows)
    summary = dict(source_sha256=sha(code/'player/solution.cpp'), engine_sha256=sha(code/'src/framework.cpp'),
                   valid_replays=len(rows)*2, cost_repetitions=1, variants={})
    for variant in ('greedy','strategy'):
        selected = [r for r in rows if r['variant']==variant]
        summary['variants'][variant] = {k: sum(r.get(k,0) for r in selected) for k in columns[5:]}
    (args.output/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps(summary,ensure_ascii=False,indent=2))


if __name__ == '__main__':
    main()
