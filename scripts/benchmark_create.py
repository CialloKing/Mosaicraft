"""Time the installed create command, not an instrumented MosaicEngine invocation."""
import argparse
import json
import random
import sqlite3
import subprocess
import time
import uuid
from contextlib import closing
from pathlib import Path

from benchmark_backends import balanced_orders, save, summarize
from study_paths import digest, readonly_database

CASES = {900: (270, 480, 150), 3600: (540, 960, 150),
         57600: (2160, 3840, 150), 14400: (1080, 1920, 600)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline', 'candidate', 'prepared', 'input', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--backend', choices=('cpu', 'cuda'), required=True)
    p.add_argument('--tiles', type=int, nargs='+', choices=CASES, default=list(CASES))
    p.add_argument('--threads', type=int, default=0)
    p.add_argument('--rounds', type=int, default=8)
    p.add_argument('--warmups', type=int, default=2)
    a = p.parse_args()
    if a.rounds < 1 or a.warmups < 0 or len(set(a.tiles)) != len(a.tiles):
        p.error('invalid sample counts or duplicate cases')
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
                input_sha256=digest(a.input), snapshot_sha256=digest(src), complete=False)
    if manifest.get('snapshot_sha256', data['snapshot_sha256']) != data['snapshot_sha256']:
        raise ValueError('snapshot identity mismatch')
    rng = random.Random(29)
    orders = {n: balanced_orders(2, a.rounds, rng) for n in a.tiles}
    hashes = {}
    try:
        for phase, rounds in [('warmup',a.warmups), ('measured',a.rounds)]:
            for r in range(rounds):
                for n in (a.tiles if r%2 == 0 else reversed(a.tiles)):
                    w,h,c = CASES[n]
                    for idx in orders[n][r%a.rounds]:
                        name = list(bins)[idx]
                        tag = f'{phase}-{r}-{n}-{name}'
                        image = out/f'{n}-{name}.png'
                        with readonly_database(src) as original, closing(sqlite3.connect(db)) as dest:
                            original.backup(dest)
                        cmd = [bins[name],'create',a.input.resolve(),'-l',db,'-o',image,'--backend',a.backend,
                               '--score-mode','fast','--threads',a.threads,'--target-size',f'{w}x{h}',
                               '--match-tile','9x16','--output-tile','9x16','--l-range',255,'--seed',42,
                               '--random-top',10,'--candidates',c,'--overwrite','--json']
                        with (out/f'{tag}.json').open('wb') as stdout, (out/f'{tag}.log').open('wb') as stderr:
                            start = time.perf_counter()
                            subprocess.run(list(map(str,cmd)), stdout=stdout, stderr=stderr, check=True)
                            wall = (time.perf_counter()-start)*1000
                        res = json.loads((out/f'{tag}.json').read_bytes()); rt = res['runtime']
                        if (not res['ok'] or res['warningCount'] or rt['matched'] != n or rt['failed'] or
                            rt['device'].split(':')[0] != a.backend or rt['scoreMode'] != 'fast'):
                            raise RuntimeError(tag+': incomplete or unexpected execution')
                        sha = digest(image)
                        sample = dict(phase=phase, round=r, case=str(n), variant=name, png_sha256=sha,
                                      device=rt['device'], kernel=rt['kernelVersion'], metrics=dict(rt['metrics'],processWallMs=wall))
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
