"""Paired continuous Web UI previews against a prepared private library (never a live library)."""
import argparse
import hashlib
import json
import os
import random
import socket
import subprocess
import time
from pathlib import Path
from urllib.error import URLError
from urllib.request import ProxyHandler, Request, build_opener

from benchmark_backends import balanced_orders, save, summarize
from benchmark_create import CASES
from study_paths import digest

CASES = {**CASES, 102400: (2880, 5120, 150)}
MODES = ('standard', 'scores', 'features', 'both', 'standard+stamps', 'both+stamps', 'auto',
         '512-128-64+stamps', '512-128-192+stamps', '1024-256-384+stamps')
ENDPOINT = '/api/mosaic/preview'
STEPS = {
    'cold': {}, 'top': {'topNRandom': 5}, 'neighbor': {'neighborPenalty': 60, 'neighborWindow': 100},
    'weight': {'labWeight': .50}, 'weight-top': {'topNRandom': 8}, 'candidate': {'candidates': 100}, 'brightness': {'lRange': 30},
    'grid': {'tileW': 12},
}


def hash_json(value):
    return hashlib.sha256(json.dumps(value, separators=(',', ':')).encode()).hexdigest()


def session(a, exe, experiment, db, folder, w, h, c):
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    opener = build_opener(ProxyHandler({}))

    def call(path, body=None, method=None, raw=False):
        req = Request(f'http://localhost:{port}' + path, method=method,
                      data=None if body is None else json.dumps(body).encode(),
                      headers={'Content-Type': 'application/json'})
        with opener.open(req, timeout=30) as response:
            value = response.read() if raw else json.load(response)
        if not raw and not value['ok']:
            raise ValueError(value)
        return value

    settings = dict(inputPath=str(a.input.resolve()), dbPath=str(db), outputPath=str(folder / 'unused.png'),
                    format='png', backend=a.backend, tileW=9, tileH=16, nativeTileW=9, nativeTileH=16,
                    outW=w, outH=h, threads=a.threads, candidates=c, topNRandom=10, lRange=255,
                    generatePreview=False, scoreMode='fast', benchmark=a.diagnose)
    env = os.environ.copy()
    cache_mode, _, stamp_mode = experiment.partition('+')
    env['MOSAICRAFT_BENCH_PREVIEW_CACHE'] = cache_mode
    if cache_mode == 'auto':
        env.pop('MOSAICRAFT_BENCH_PREVIEW_STAMPS', None)
    else:
        env['MOSAICRAFT_BENCH_PREVIEW_STAMPS'] = '1' if stamp_mode == 'stamps' else '0'
    with (folder / 'server.log').open('wb') as log:
        proc = subprocess.Popen([exe, str(port), '--no-open'], cwd=folder, env=env, stdout=log,
                                stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        try:
            for _ in range(100):
                if proc.poll() is not None:
                    raise RuntimeError('server exited')
                try:
                    call('/api/info')
                    break
                except URLError:
                    time.sleep(.1)
            else:
                raise TimeoutError('server startup')
            rows = []
            previous = pinned = None
            for i, step in enumerate(a.steps):
                settings.update(STEPS[step])
                if a.cold_each and previous:
                    call(previous, method='DELETE')
                    previous = None
                start = time.perf_counter()
                submitted = call(ENDPOINT, dict(request=settings, seed=42))['preview']
                path = ENDPOINT + '/' + submitted['id']
                while True:
                    state = call(path)['preview']
                    if state['state'] not in ('queued', 'running'):
                        break
                    if time.perf_counter() - start > 180:
                        raise TimeoutError(path)
                    time.sleep(.025)
                wall = (time.perf_counter() - start) * 1000
                if state['state'] != 'succeeded' or state['runtime']['device'].split(':')[0] != a.backend:
                    save(folder / 'failure.json', state)
                    raise ValueError(f'{folder}: wrong backend or preview failed')
                signature = {'selection': hash_json(state['imageIds'])}
                if a.verify_artifacts:
                    signature['reference'] = hashlib.sha256(call(path + '/image/reference', raw=True)).hexdigest()
                    recommendations = []
                    for n in (0, len(state['imageIds']) // 2, len(state['imageIds']) - 1):
                        images = call(path + f'/recommendations?position={n}')['recommendations']['images']
                        recommendations.append([img['id'] for img in images])
                    signature['recommendations'] = hash_json(recommendations)
                rss = {}
                if a.diagnose:
                    import psutil
                    mem = psutil.Process(proc.pid).memory_info()
                    rss = dict(resident_bytes=mem.rss, peak_working_set_bytes=getattr(mem, 'peak_wset', None))
                row = dict(metrics=dict(state['metrics'], processWallMs=wall), signature=signature,
                           device=state['runtime']['device'], features=state['featureBackend'], scorer=state['scoreBackend'], **rss)
                rows.append(row)
                if previous and previous != pinned:
                    call(previous, method='DELETE')
                previous = path
                if a.pin_first and not i:
                    call(ENDPOINT + '/pin', dict(slot='A', previewId=state['id']))
                    pinned = path
                if pinned and i:
                    saved = call(pinned)['preview']
                    if hash_json(saved['imageIds']) != rows[0]['signature']['selection']:
                        raise ValueError('pinned preview changed')
                save(folder / f'{step}.json', row)
                print(folder.name, step, round(wall), [row['metrics'].get(k) for k in
                      ('previewScoresReused', 'previewTileFeaturesReused', 'previewCandidatesReused')], flush=True)
            return rows
        finally:
            proc.terminate()
            proc.wait(timeout=30)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('baseline', 'candidate', 'prepared', 'input', 'output'):
        parser.add_argument('--' + key, type=Path, required=True)
    parser.add_argument('--backend', choices=('cpu', 'cuda'), default='cpu')
    parser.add_argument('--baseline-mode', choices=MODES, default='standard')
    parser.add_argument('--variants', nargs='+', choices=MODES, default=['both'])
    parser.add_argument('--candidates', type=int, help='override the ANN count for each grid')
    parser.add_argument('--tiles', nargs='+', type=int, choices=CASES, default=[57600])
    parser.add_argument('--steps', nargs='+', choices=STEPS, default=['cold', 'top', 'neighbor', 'weight'])
    parser.add_argument('--rounds', type=int, default=8)
    parser.add_argument('--warmups', type=int, default=2)
    parser.add_argument('--threads', type=int, default=0)
    parser.add_argument('--diagnose', action='store_true', help='enable benchmark metrics and sample server memory (psutil)')
    parser.add_argument('--verify-artifacts', action='store_true', help='untimed reference JPEG and representative recommendations')
    parser.add_argument('--cold-each', action='store_true', help='close each previous preview before submitting the next')
    parser.add_argument('--pin-first', action='store_true', help='diagnostic: retain the cold selection in A')
    a = parser.parse_args()
    if a.pin_first and (not a.diagnose or a.cold_each):
        parser.error('--pin-first requires diagnostics and a shared session')
    if a.rounds < 1 or a.warmups < 0 or a.steps[0] != 'cold' or len(set(a.steps)) != len(a.steps):
        parser.error('invalid sampling configuration')
    if a.candidates is not None and not 1 <= a.candidates <= 600:
        parser.error('--candidates must be between 1 and 600')
    root = a.prepared.resolve()
    db = root / 'library/snapshot.db'
    snapshot = digest(db)
    manifest = json.loads((root / 'report.json').read_bytes())
    if not manifest['complete'] or manifest.get('snapshot_sha256', snapshot) != snapshot:
        parser.error('requires the original prepared private benchmark snapshot')
    output = a.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    variants = dict(baseline=a.baseline.resolve(), **{k: a.candidate.resolve() for k in a.variants})
    modes = {name: name if name != 'baseline' else a.baseline_mode for name in variants}
    counts = {n: a.candidates or CASES[n][2] for n in a.tiles}
    report = dict(complete=False, reference='baseline',
                  variants=[dict(id=k, cache_mode=modes[k], sha256=digest(v)) for k, v in variants.items()],
                  candidate_counts=counts,
                  cases=[dict(id=f'{n}-{s}') for n in a.tiles for s in a.steps], samples=[], differences=[],
                  backend=a.backend, threads=a.threads, input_sha256=digest(a.input), snapshot_sha256=snapshot,
                  exploratory=a.rounds < 8 or a.warmups < 2, diagnose=a.diagnose, cold_each=a.cold_each, pin_first=a.pin_first)
    expected = {}
    orders = balanced_orders(len(variants), a.rounds, random.Random(29))
    for phase, count in [('warmup', a.warmups), ('measured', a.rounds)]:
        for r in range(count):
            for n in a.tiles:
                for index in orders[r % len(orders)]:
                    name, exe = list(variants.items())[index]
                    folder = output / f'{phase}-{r}-{n}-{name}'
                    folder.mkdir()
                    w, h, _ = CASES[n]
                    for step, row in zip(a.steps, session(a, exe, modes[name], db, folder, w, h, counts[n])):
                        case = f'{n}-{step}'
                        row.update(phase=phase, round=r, case=case, variant=name)
                        report['samples'].append(row)
                        if expected.setdefault(case, row['signature']) != row['signature']:
                            report['differences'].append(dict(case=case, round=r, variant=name))
                    save(output / 'report.json', report)
    if digest(db) != snapshot:
        raise RuntimeError('private snapshot changed')
    report.update(complete=True, summary=summarize(report))
    save(output / 'report.json', report)
    return bool(report['differences'])


if __name__ == '__main__':
    raise SystemExit(main())
