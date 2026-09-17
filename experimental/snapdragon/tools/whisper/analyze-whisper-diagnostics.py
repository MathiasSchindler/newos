import argparse
import collections
import csv
import json
import math
import pathlib
import re
import statistics
import tempfile


PHASES = {0: 'token', 1: 'self', 2: 'cross_mlp', 3: 'projection', 4: 'selection', 5: 'log_mel'}
UNITS = {1: 'us', 2: 'bytes', 3: 'cycles', 4: 'count', 5: 'object', 6: 'none'}


def distribution(values):
    values = sorted(values)
    if not values:
        return {'count': 0}
    return dict(count=len(values), total=sum(values), mean=statistics.mean(values),
                median=statistics.median(values),
                p95=values[max(0, math.ceil(len(values) * .95) - 1)],
                p99=values[max(0, math.ceil(len(values) * .99) - 1)], maximum=values[-1])


def family(name):
    return re.sub(r'_l\d+_', '_layer_', name)


def load(path):
    with path.open(newline='', encoding='utf-8') as source:
        rows = list(csv.DictReader(source))
    for row in rows:
        for key in row:
            if key not in ('kind', 'name'):
                row[key] = int(row[key])
    metadata = [row for row in rows if row['kind'] == 'meta']
    if len(metadata) != 1 or metadata[0]['id'] != 1:
        raise ValueError('Unsupported or missing diagnostics schema')
    return metadata[0], rows


def analyze(path):
    meta, rows = load(path)
    frequency, origin = meta['graph'], meta['window']
    scale = 1e6 / frequency
    records = {row['id']: row for row in rows if row['kind'] in ('graph', 'phase')}
    graphs = [row for row in records.values() if row['kind'] == 'graph']
    phases = [row for row in records.values() if row['kind'] == 'phase']
    events = [row for row in rows if row['kind'] == 'event']
    if len(records) != meta['end'] or len(events) != meta['user_100ns']:
        raise ValueError('Truncated diagnostics file')
    if any(row['end'] < row['start'] for row in records.values()):
        raise ValueError('Unbalanced trace phase')
    by_graph = collections.defaultdict(list)
    by_family = collections.defaultdict(list)
    positions = collections.defaultdict(list)
    graph_samples = collections.defaultdict(lambda: collections.defaultdict(list))
    sample_pairs = collections.defaultdict(dict)
    event_types = collections.defaultdict(list)
    node_cycles = collections.defaultdict(lambda: collections.defaultdict(list))
    windows = collections.defaultdict(lambda: collections.defaultdict(list))
    trace = []
    attempts = {}
    attempt_counts = collections.defaultdict(int)
    for row in sorted(phases, key=lambda item: item['start']):
        if row['phase'] == 0:
            if row['position'] == 0:
                attempt_counts[row['window']] += 1
            attempts[(row['window'], row['step'])] = attempt_counts[row['window']]
    for row in graphs:
        duration = (row['end'] - row['start']) * scale
        by_graph[row['name']].append(duration)
        by_family[family(row['name'])].append(duration)
        windows[str(row['window'])][family(row['name'])].append(duration)
        if row['position'] != 0xffffffff:
            positions[f"{family(row['name'])}:positions-{row['position'] // 64 * 64}-{row['position'] // 64 * 64 + 63}"].append(duration)
        trace.append(dict(name=row['name'], cat='QNN host call', ph='X', pid=1, tid=3,
                          ts=(row['start'] - origin) * scale, dur=duration,
                          args=dict(attempt=attempts.get((row['window'], row['step']), 0),
                                    **{key: row[key] for key in ('window', 'step', 'position', 'sampled', 'status')})))
    by_phase = collections.defaultdict(list)
    cpu_phases = collections.defaultdict(lambda: [0, 0])
    for row in phases:
        label = PHASES.get(row['phase'], str(row['phase']))
        duration = (row['end'] - row['start']) * scale
        by_phase[label].append(duration)
        cpu_phases[label][0] += row['user_100ns'] / 1e7
        cpu_phases[label][1] += row['kernel_100ns'] / 1e7
        trace.append(dict(name=label, cat='decoder phase', ph='X', pid=1,
                          tid=1 if row['phase'] == 0 else 2,
                          ts=(row['start'] - origin) * scale, dur=duration,
                          args={key: row[key] for key in ('window', 'step', 'position', 'layer')}))
    for event in events:
        record = records.get(event['graph'])
        if event['status'] or record is None:
            continue
        event_type, unit, value = event['step'], event['position'], event['layer']
        label = f"{event_type}:{UNITS.get(unit, str(unit))}:{event['name']}"
        event_types[label].append(value)
        if event_type == 404 and unit == 3:
            node_name = family(re.sub(r':OpId_\d+', '', event['name']))
            node_cycles[family(record['name'])][node_name].append(value)
        if event['window'] == 0xffffffff:
            graph_samples[family(record['name'])][str(event_type)].append(value)
            if unit == 1:
                sample_pairs[record['id']][event_type] = value
    overhead = collections.defaultdict(lambda: collections.defaultdict(list))
    for record_id, values in sample_pairs.items():
        record = records[record_id]
        target = overhead[family(record['name'])]
        target['host_us'].append((record['end'] - record['start']) * scale)
        if 3012 in values:
            target['accelerator_excluding_wait_us'].append(values[3012])
            target['host_minus_accelerator_us'].append((record['end'] - record['start']) * scale - values[3012])
        if 3004 in values and 3012 in values:
            target['accelerator_wait_us'].append(values[3004] - values[3012])
        if 3001 in values and 3002 in values:
            target['host_rpc_minus_htp_rpc_us'].append(values[3001] - values[3002])
    gaps = []
    ordered = sorted(graphs, key=lambda row: row['start'])
    for previous, current in zip(ordered, ordered[1:]):
        if previous['window'] == current['window']:
            gaps.append((current['start'] - previous['end']) * scale)
    cpu_threads = []
    thread_path = path.with_name('threads.csv')
    if thread_path.exists():
        grouped = collections.defaultdict(list)
        with thread_path.open(newline='', encoding='utf-8-sig') as source:
            for row in csv.DictReader(source):
                grouped[row['thread_id']].append(row)
        for thread_id, samples in grouped.items():
            first, last = samples[0], samples[-1]
            user = (int(last['user_100ns']) - int(first['user_100ns'])) / 1e7
            kernel = (int(last['kernel_100ns']) - int(first['kernel_100ns'])) / 1e7
            cpu_threads.append(dict(thread_id=int(thread_id), user_seconds=user, kernel_seconds=kernel,
                                    samples=len(samples), states=dict(collections.Counter(row['state'] for row in samples)),
                                    wait_reasons=dict(collections.Counter(row['wait_reason'] for row in samples if row['wait_reason']))))
        cpu_threads.sort(key=lambda row: row['user_seconds'] + row['kernel_seconds'], reverse=True)
    summary = dict(
        schema=1, model_id=meta['status'], decoder_mask=meta['sampled'], level=meta['step'],
        records=len(records), events=len(events), dropped_records=meta['position'],
        graph_errors=[dict(id=row['id'], name=row['name'], status=row['status'], sampled=row['sampled']) for row in graphs if row['status']],
        dropped_events=meta['layer'], diagnostic_errors=meta['phase'],
        profile_bookkeeping_ms=meta['start'] * scale / 1000,
        sample_stride_per_graph=meta['kernel_100ns'],
        graph_us={key: distribution(values) for key, values in by_graph.items()},
        attempts_per_window=dict(attempt_counts),
        family_us={key: distribution(values) for key, values in by_family.items()},
        window_family_us={window: {name: distribution(values) for name, values in groups.items()} for window, groups in windows.items()},
        position_bucket_us={key: distribution(values) for key, values in positions.items()},
        phase_us={key: distribution(values) for key, values in by_phase.items()},
        phase_process_cpu_seconds={key: dict(user=values[0], kernel=values[1]) for key, values in cpu_phases.items() if key in ('token', 'selection')},
        inter_graph_gap_us=distribution(gaps),
        sampled_device_events={key: distribution(values) for key, values in event_types.items()},
        sampled_device_events_by_family={key: {event: distribution(values) for event, values in types.items()} for key, types in graph_samples.items()},
        sampled_host_device_breakdown={key: {metric: distribution(values) for metric, values in metrics.items()} for key, metrics in overhead.items()},
        sampled_node_cycles_by_family={key: {node: distribution(values) for node, values in nodes.items()} for key, nodes in node_cycles.items()},
        threads=cpu_threads,
        caveats=['Device events are sampled, not total device utilization.',
                 'Event hierarchy times overlap; do not sum parent and child events.',
                 'Host-call durations and gaps include instrumentation effects.',
                 'Token CPU includes selection CPU; phase CPU accounting is process-wide and coarsely scheduled.',
                 'Thread samples omit CPU before the first/after the last observation and short-lived threads.',
                 'Thread states are snapshots, not a scheduling trace or stack profile.'])
    return summary, dict(traceEvents=trace, displayTimeUnit='ms')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('input', type=pathlib.Path, nargs='?')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--overview', action='store_true')
    parser.add_argument('--collection', type=pathlib.Path)
    args = parser.parse_args()
    if args.collection:
        runs = json.loads((args.collection / 'runs.json').read_text(encoding='utf-8-sig'))
        if isinstance(runs, dict):
            runs = [runs]
        grouped = collections.defaultdict(list)
        for run in runs:
            grouped[run['diagnostics']].append(run)
        result = {mode: {metric: distribution([run[metric] for run in values])
                         for metric in ('wall_seconds', 'process_cpu_seconds', 'window_total_ms',
                                        'context_restore_ms', 'npu_host_call_ms', 'retry_steps')}
                  for mode, values in grouped.items()}
        result['transcript_hashes'] = sorted(set(run['transcript_sha256'] for run in runs))
        (args.collection / 'aggregate.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        report = ['# Medium diagnostic measurements', '',
              f'Configuration: Medium, fused/self/logits, {runs[0]["audio_seconds"]:g}-second reference audio.', '',
                  f'Completed runs: {len(runs)}. Distinct transcript hashes: {len(result["transcript_hashes"])}.', '',
                  '| Mode | Runs | Median wall seconds | Median CPU seconds | Median window seconds |',
                  '| --- | ---: | ---: | ---: | ---: |']
        for mode, values in grouped.items():
            metrics = result[mode]
            report.append(f'| {mode} | {len(values)} | {metrics["wall_seconds"]["median"]:.3f} | '
                          f'{metrics["process_cpu_seconds"]["median"]:.3f} | {metrics["window_total_ms"]["median"] / 1000:.3f} |')
        if 'off' in result:
            report += ['', 'Relative median wall time versus uninstrumented:']
            for mode in ('trace', 'basic'):
                if mode in result:
                    overhead = 100 * (result[mode]['wall_seconds']['median'] / result['off']['wall_seconds']['median'] - 1)
                    report.append(f'- {mode}: {overhead:+.2f}%. This includes run-to-run scheduling/power variance, not just instrumentation.')
        report += ['', '## Coverage', '',
                   '- QNN execution events: collected, with sampled host/RPC/accelerator/wait decomposition.',
                   '- Token/layer timeline: collected, with retry/position labels and graph gaps.',
                   '- Latency distributions: collected per graph, family, position bucket and window.',
                   '- CPU attribution: process CPU by phase and 250 ms thread snapshots collected. Kernel stacks/context switches remain blocked pending elevated WPR capture.',
                   '- Device counters: detailed operator/accelerator cycles and acquisition times collected separately. Measured DDR bandwidth, live clock, hardware occupancy and thermal throttling are not available from the collected events.', '',
                   '## Representative basic run', '']
        basic_runs = grouped.get('basic', [])
        if basic_runs:
            selected = basic_runs[-1]
            directory = args.collection / f'run-{selected["repetition"]:02d}-basic'
            diagnostic = json.loads((directory / 'diagnostics.summary.json').read_text(encoding='utf-8'))
            report += [f'Source: {directory.name}. {diagnostic["records"]} intervals, {diagnostic["events"]} device events; '
                       f'{diagnostic["dropped_records"]} dropped intervals, {diagnostic["dropped_events"]} dropped events, '
                       f'{diagnostic["diagnostic_errors"]} diagnostic errors.', '',
                       '| Graph family | Calls | Median host us | p95 host us | p99 host us | Total host s |',
                       '| --- | ---: | ---: | ---: | ---: | ---: |']
            for name, metrics in diagnostic['family_us'].items():
                report.append(f'| {name} | {metrics["count"]} | {metrics["median"]:.1f} | {metrics["p95"]:.1f} | '
                              f'{metrics["p99"]:.1f} | {metrics["total"] / 1e6:.3f} |')
            report += ['', '| Sampled family | Samples | Median host us | Median accelerator excluding wait us | Median host minus accelerator us |',
                       '| --- | ---: | ---: | ---: | ---: |']
            for name, metrics in diagnostic['sampled_host_device_breakdown'].items():
                if 'accelerator_excluding_wait_us' in metrics:
                    report.append(f'| {name} | {metrics["host_us"]["count"]} | {metrics["host_us"]["median"]:.1f} | '
                                  f'{metrics["accelerator_excluding_wait_us"]["median"]:.1f} | '
                                  f'{metrics["host_minus_accelerator_us"]["median"]:.1f} |')
            report += ['', 'Process CPU attributed to decoder phases (nested, do not add together):']
            for name, metrics in diagnostic['phase_process_cpu_seconds'].items():
                report.append(f'- {name}: {metrics["user"] + metrics["kernel"]:.3f} CPU-seconds.')
            report += [f'- Whole process: {selected["process_cpu_seconds"]:.3f} CPU-seconds.',
                       f'- Decoder steps: {selected["decoder_steps"]}; prefix-reused steps: {selected["prefix_reused_steps"]}; '
                       f'retry-step estimate: {selected["retry_steps"]}.']
        report += ['', '## Interpretation limits', '',
                   'Detailed profiling runs continuously because toggling it caused DSP DMA6006 errors. Its timing is intrusive and is not included in the latency comparison above.',
                   'Device-event samples are not device occupancy. Host minus accelerator includes SDK/RPC/transfer/scheduling effects and does not identify any one of them in isolation.',
                   'Thread CPU snapshots omit short-lived threads and time outside their observation interval. Phase CPU uses coarse process accounting; selection is included in token CPU.',
                   'Normal and trace/basic runs must preserve transcript hashes. Review inventory.json for runtime identity, power plan, AC/battery state and unavailable thermal access.', '']
        (args.collection / 'report.md').write_text('\n'.join(report), encoding='utf-8')
        print(json.dumps(result, indent=2))
        return
    if args.self_test:
        assert distribution([]) == {'count': 0}
        assert distribution([1, 2, 3, 100])['p95'] == 100
        assert distribution([1, 2, 3, 100])['median'] == 2.5
        assert family('medium_decoder_l23_self_projection_graph') == 'medium_decoder_layer_self_projection_graph'
        with tempfile.TemporaryDirectory() as temporary:
            fixture = pathlib.Path(temporary) / 'fixture.csv'
            fixture.write_text(
                'kind,id,graph,window,step,position,layer,phase,start,end,user_100ns,kernel_100ns,status,sampled,name\n'
                'meta,1,10000000,100,1,0,0,0,20,3,1,127,4,28,schema1\n'
                'phase,0,4294967295,1,1,0,4294967295,0,100,1100,10000,20000,0,0,\n'
                'graph,1,0,1,1,0,4294967295,0,200,700,0,0,0,1,test_graph\n'
                'phase,2,4294967295,1,1,0,4294967295,4,800,1000,1000,2000,0,0,\n'
                'event,0,1,4294967295,3012,1,30,0,0,0,0,0,0,0,"quoted,""event"""\n',
                encoding='utf-8')
            summary, trace = analyze(fixture)
            assert summary['family_us']['test_graph']['median'] == 50
            assert summary['sampled_host_device_breakdown']['test_graph']['host_minus_accelerator_us']['median'] == 20
            assert summary['attempts_per_window'] == {1: 1}
            assert summary['phase_process_cpu_seconds']['token']['kernel'] == .002
            assert trace['traceEvents'][0]['ts'] == 10
            assert summary['sampled_device_events']['3012:us:quoted,"event"']['count'] == 1
            fixture.write_text(fixture.read_text().replace('20,3,1,127', '20,4,1,127'), encoding='utf-8')
            try:
                analyze(fixture)
            except ValueError:
                pass
            else:
                raise AssertionError('Truncated trace accepted')
        print('PASS diagnostic analyzer checks')
        return
    if args.input is None:
        parser.error('input is required')
    summary, trace = analyze(args.input)
    args.input.with_suffix('.summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    args.input.with_suffix('.trace.json').write_text(json.dumps(trace, separators=(',', ':')), encoding='utf-8')
    print(json.dumps({key: summary[key] for key in ('records', 'events', 'dropped_records', 'dropped_events', 'diagnostic_errors', 'profile_bookkeeping_ms', 'family_us', 'phase_process_cpu_seconds')}, indent=2))
    if args.overview:
        print(json.dumps(dict(
            sampled_breakdown=summary['sampled_host_device_breakdown'],
            non_node_events={key: value for key, value in summary['sampled_device_events'].items() if not key.startswith('404:')},
            top_node_cycles=sorted(((key, value) for key, value in summary['sampled_device_events'].items() if key.startswith('404:cycles:')), key=lambda item: item[1]['median'], reverse=True)[:15],
            decoder_node_cycles={key: value for key, value in summary['sampled_node_cycles_by_family'].items() if 'decoder' in key},
            top_threads=summary['threads'][:15], attempts=summary['attempts_per_window']), indent=2))
    if summary['dropped_records'] or summary['dropped_events'] or summary['diagnostic_errors'] or summary['graph_errors']:
        raise SystemExit('Incomplete diagnostics; inspect summary')


if __name__ == '__main__':
    main()