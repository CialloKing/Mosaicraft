"""Time the installed create command, not an instrumented MosaicEngine invocation."""
import argparse
import json
import os
import random
import shutil
import sqlite3
import subprocess
import time
import uuid
from contextlib import closing
from pathlib import Path

from benchmark_backends import balanced_orders, extend_pairs, save, summarize
from study_paths import digest, readonly_database

CASES = {900: (270, 480, 150), 3600: (540, 960, 150),
         57600: (2160, 3840, 150), 14400: (1080, 1920, 600)}


# Isolate the paired children: an exported setting must never silently affect both variants.
SETTINGS = {
    'kernel': ('scalar', 'avx2', 'avx512', 'neon'),
    'batch': ('1024', '2048'), 'host_mib': ('256', '512', '1024'),
    'feature_mib': ('256', '512', '1024'), 'slots': ('65536', '262144', '1048576'),
    'score_grain': ('1', '4', '8', '16'), 'feature_grain': ('1', '4', '8', '16'),
    'prefetch': ('0', '1'), 'share': ('0', '1'), 'ways': ('1', '2'), 'planar': ('0', '1'), 'reuse_tiles': ('0', '1'),
}
PREFIX = 'MOSAICRAFT_BENCH_'


def setting(text):
    key, _, value = text.partition('=')
    if value not in SETTINGS.get(key, ()):
        raise argparse.ArgumentTypeError(f'invalid trial setting {text!r}; choices: {SETTINGS}')
    return key, value


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline', 'candidate', 'prepared', 'input', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--backend', choices=('cpu', 'cuda'), required=True)
    p.add_argument('--tiles', type=int, nargs='+', choices=CASES, default=list(CASES))
    p.add_argument('--threads', type=int, default=0)
    p.add_argument('--candidate-threads', type=int, help='compare a thread budget against --threads in paired runs')
    p.add_argument('--rounds', type=int, default=8)
    p.add_argument('--warmups', type=int, default=2)
    p.add_argument('--extend', type=Path, help='completed paired report; --rounds is the combined measured count')
    for side in ('baseline', 'candidate'):
        p.add_argument('--' + side + '-setting', type=setting, action='append', default=[], metavar='KEY=VALUE')
    a = p.parse_args()
    settings = {}; environments = {}
    for side in ('baseline', 'candidate'):
        pairs = getattr(a, side + '_setting'); cfg = dict(pairs)
        if len(cfg) != len(pairs) or (cfg and a.backend != 'cpu'):
            p.error('CPU trial settings must be unique and used with --backend cpu')
        settings[side] = cfg
        environments[side] = {k:v for k,v in os.environ.items() if not k.upper().startswith(PREFIX)}
        environments[side].update({PREFIX+k.upper():v for k,v in cfg.items()})
    if a.rounds < 1 or a.warmups < 0 or len(set(a.tiles)) != len(a.tiles):
        p.error('invalid sample counts or duplicate cases')
    candidate_threads = a.threads if a.candidate_threads is None else a.candidate_threads
    if not all(0 <= n <= 256 for n in (a.threads, candidate_threads)):
        p.error('thread counts must be in 0..256')
    prepared = a.prepared.resolve()
    manifest = json.loads((prepared/'report.json').read_bytes())
    src = prepared/'library'/'snapshot.db'
    if not manifest['complete'] or not src.is_file() or not (src.parent/'features'/'lib.ann196').is_file():
        p.error('requires a completed private benchmark and its prepared index')
    # A unique sibling keeps relative resource paths intact; create must never see the source DB.
    db = src.with_name(f'perf-create-{uuid.uuid4().hex}.db')
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    bins = {k: getattr(a, k).resolve() for k in ('baseline', 'candidate')}
    data = dict(reference='baseline', variants=[dict(id=k, sha256=digest(v)) for k,v in bins.items()],
                cases=[dict(id=str(n)) for n in a.tiles], samples=[], backend=a.backend, threads=a.threads,
                input_sha256=digest(a.input), snapshot_sha256=digest(src), complete=False, exploratory=a.rounds < 8 or a.warmups < 2)
    if any(settings.values()):
        data['cpu_settings'] = settings
    if candidate_threads != a.threads:
        data['candidate_threads'] = candidate_threads
    if manifest.get('snapshot_sha256', data['snapshot_sha256']) != data['snapshot_sha256']:
        raise ValueError('snapshot identity mismatch')
    offset,warm_offset = extend_pairs(data,a.extend,a.rounds)
    hashes = {}
    for sample in data['samples']:
        if sample['variant'] == 'baseline':
            hashes.setdefault(int(sample['case']), sample['png_sha256'])
    rng = random.Random(29 + offset)
    added = a.rounds - offset
    orders = {n: balanced_orders(2, added, rng) for n in a.tiles}
    try:
        for phase, rounds, first in [('warmup',a.warmups,warm_offset), ('measured',added,offset)]:
            for r in range(first, first + rounds):
                for n in (a.tiles if r%2 == 0 else reversed(a.tiles)):
                    w,h,c = CASES[n]
                    for idx in orders[n][(r-first)%added]:
                        name = list(bins)[idx]
                        tag = f'{phase}-{r}-{n}-{name}'
                        image = out/f'{n}-{name}.png'
                        with readonly_database(src) as original, closing(sqlite3.connect(db)) as dest:
                            original.backup(dest)
                        cmd = [bins[name],'create',a.input.resolve(),'-l',db,'-o',image,'--backend',a.backend,
                               '--score-mode','fast','--threads',a.threads if name == 'baseline' else candidate_threads,
                               '--target-size',f'{w}x{h}',
                               '--match-tile','9x16','--output-tile','9x16','--l-range',255,'--seed',42,
                               '--random-top',10,'--candidates',c,'--overwrite','--json']
                        with (out/f'{tag}.json').open('wb') as stdout, (out/f'{tag}.log').open('wb') as stderr:
                            start = time.perf_counter()
                            subprocess.run(list(map(str,cmd)), stdout=stdout, stderr=stderr, env=environments[name], check=True)
                            wall = (time.perf_counter()-start)*1000
                        res = json.loads((out/f'{tag}.json').read_bytes()); rt = res['runtime']
                        if (not res['ok'] or res['warningCount'] or rt['matched'] != n or rt['failed'] or
                            rt['device'].split(':')[0] != a.backend or rt['scoreMode'] != 'fast'):
                            raise RuntimeError(tag+': incomplete or unexpected execution')
                        if (rt['fallbackReason'] or a.backend not in rt['featureBackend'] or
                                a.backend not in rt['scoreBackend'] or not rt['kernelVersion']):
                            raise RuntimeError(tag+': selected device did not run both stages')
                        for key, value in settings[name].items():
                            accepted = (rt['kernelVersion'].endswith('-' + value) if key == 'kernel'
                                        else rt['metrics'].get('trial_' + key) == int(value))
                            if not accepted:
                                raise RuntimeError(f'{tag}: executable did not confirm {key}={value}')
                        sha = digest(image)
                        # 同种子未必同图；只在墙钟之外保存每种内容一次，下一轮不得覆盖异常样本。
                        witness = out/'outputs'/(sha+'.png')
                        if not witness.exists():
                            witness.parent.mkdir(exist_ok=True)
                            shutil.copyfile(image, witness)
                        sample = dict(phase=phase, round=r, case=str(n), variant=name, png_sha256=sha,
                                      device=rt['device'], kernel=rt['kernelVersion'], feature_backend=rt['featureBackend'],
                                      score_backend=rt['scoreBackend'], metrics=dict(rt['metrics'],processWallMs=wall))
                        data['samples'].append(sample); save(out/'report.json',data)
                        if name == 'baseline' and n not in hashes:
                            hashes[n] = sha
                        print(tag, round(wall), sha[:12], flush=True)
        if digest(src) != data['snapshot_sha256'] or digest(a.input) != data['input_sha256']:
            raise RuntimeError('snapshot or target changed')
        data['different_outputs'] = [s for s in data['samples'] if s['png_sha256'] != hashes[int(s['case'])]]
        data.update(summary=summarize(data), complete=True); save(out/'report.json',data)
    finally:
        for suffix in ('','-journal','-shm','-wal','.library.lock'):
            Path(str(db)+suffix).unlink(missing_ok=True)


if __name__ == '__main__':
    main()
