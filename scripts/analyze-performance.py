"""Join guest-swap timings, thread CPU samples and pipeline logs by UTC time."""
import argparse
import csv
import json
import re
import statistics
from datetime import datetime, timedelta, timezone
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
parser.add_argument('log', type=Path)
args = parser.parse_args()
meta = json.loads((args.directory / 'sampling.json').read_text())
anchor = datetime.fromisoformat(meta['started_utc'])
frequency = meta['host_tick_frequency']

def read_csv(name):
    with (args.directory / name).open(encoding='utf-8-sig', newline='') as stream:
        return list(csv.DictReader(stream))

def tick_time(tick):
    return anchor + timedelta(seconds=(int(tick) - meta['host_tick']) / frequency)

def percentile(values, fraction):
    values = sorted(values)
    return values[min(len(values) - 1, int((len(values) - 1) * fraction))]

frames = read_csv('frames.csv')
threads = read_csv('thread-cpu.csv')
gpu = read_csv('gpu.csv')
for row in frames + threads:
    row['time'] = tick_time(row['host_tick'])
for row in gpu:
    row['time'] = datetime.fromisoformat(row['utc'])

# Runtime log timestamps use the machine's local clock: New York, UTC-4 on
# October 12, 2026. Keep the offset explicit in this evidence-specific analyzer.
local_offset = timezone(timedelta(hours=-4))
pipeline_times = []
for line in args.log.read_text(errors='replace').splitlines():
    if 'Creating graphics pipeline' not in line:
        continue
    match = re.match(r'\[([^]]+)\]', line)
    if match:
        pipeline_times.append(datetime.fromisoformat(match[1]).replace(tzinfo=local_offset))

start = frames[0]['time'].replace(microsecond=0)
end = frames[-1]['time']
bins = []
while start < end:
    stop = start + timedelta(seconds=10)
    current = [r for r in frames if start <= r['time'] < stop]
    cpu = [r for r in threads if start <= r['time'] < stop]
    engines = [float(r['utilization_percent']) for r in gpu
               if start <= r['time'] < stop and r['engine'].endswith('engtype_3d')]
    if current:
        intervals = [float(r['frame_ms']) for r in current]
        swaps = [float(r['swap_ms']) for r in current]
        sample_times = {r['host_tick'] for r in cpu}
        groups = {}
        for r in cpu:
            name = r['name']
            if name.startswith('D3D Background Thread'):
                name = 'D3D driver workers (sum)'
            groups[name] = groups.get(name, 0.0) + float(r['cpu_percent_one_core'])
        groups = {k: round(v / len(sample_times), 2) for k, v in groups.items()} if sample_times else {}
        bins.append(dict(
            utc=start.isoformat(), local=start.astimezone(local_offset).strftime('%H:%M:%S'),
            frames=len(current), guest_swap_fps=round(1000 / statistics.mean(intervals), 2),
            median_frame_ms=round(statistics.median(intervals), 2),
            p95_frame_ms=round(percentile(intervals, .95), 2),
            median_swap_ms=round(statistics.median(swaps), 3),
            new_pipeline_starts=sum(start <= t < stop for t in pipeline_times),
            gpu_3d_percent=round(statistics.mean(engines), 2) if engines else None,
            cpu_percent_one_core=groups))
    start = stop

result = dict(
    metric='Guest swap intervals; not independent displayed-frame measurements',
    configuration='Release, RTV, bindless=false, async=true, DRED=true, warmed cache',
    caveats=['Loading/menu and gameplay all appear in the timeline.',
             'Pipeline-start logs do not measure driver compilation completion.',
             'CPU percentage 100 equals one logical processor.',
             'DRED and alternate resource binding may affect performance.',
             'Buffered frame logging may omit the final few seconds after hard exit.'],
    bins=bins)
(args.directory / 'summary.json').write_text(json.dumps(result, indent=2))
print('Local time | swap FPS | p95 ms | new pipelines | pipeline CPU | driver CPU | GPU 3D')
for b in bins:
    c = b['cpu_percent_one_core']
    print(f"{b['local']} | {b['guest_swap_fps']:6.2f} | {b['p95_frame_ms']:6.1f} | "
          f"{b['new_pipeline_starts']:3d} | {c.get('D3D12 Pipelines', 0):6.1f} | "
          f"{c.get('D3D driver workers (sum)', 0):6.1f} | {b['gpu_3d_percent']}")
