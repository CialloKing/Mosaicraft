"""Paired real render timings on an unchanged portable layout (never opens a library)."""
import argparse
import hashlib
import json
import random
import subprocess
import time
from pathlib import Path
from xml.etree import ElementTree

from PIL import Image
from benchmark_backends import balanced_orders, save, summarize
from study_paths import digest


def pixels(path, expected_size):
    h = hashlib.sha256()
    with Image.open(path) as im:
        if im.size != expected_size:
            raise ValueError(f'wrong dimensions: {im.size}, expected {expected_size}')
        for y in range(0, im.height, 64):
            h.update(im.crop((0, y, im.width, min(y + 64, im.height))).convert('RGB').tobytes())
    return h.hexdigest()


def deepzoom_pixels(path, expected_size):
    manifest = path / 'index.dzi'
    with manifest.open('rb') as source:
        descriptor = ElementTree.parse(source).getroot()
    size = descriptor.find('{*}Size')
    if descriptor.attrib != {'Format': 'jpg', 'Overlap': '0', 'TileSize': '256'} or (
            int(size.attrib['Width']), int(size.attrib['Height'])) != expected_size:
        raise ValueError('wrong Deep Zoom descriptor')
    width, height = expected_size
    levels = [(width, height)]
    while width > 1 or height > 1:
        width, height = (width + 1) // 2, (height + 1) // 2
        levels.append((width, height))
    pyramid = path / 'index_files'
    if {p.name for p in pyramid.iterdir()} != {str(i) for i in range(len(levels))}:
        raise ValueError('wrong Deep Zoom level count')
    h = hashlib.sha256(manifest.read_bytes())
    h.update((path / 'index.html').read_bytes())
    for level, (width, height) in enumerate(reversed(levels)):
        folder = pyramid / str(level)
        columns, rows = (width + 255) // 256, (height + 255) // 256
        if sum(1 for _ in folder.iterdir()) != columns * rows:
            raise ValueError(f'wrong Deep Zoom tile count at level {level}')
        for y in range(rows):
            for x in range(columns):
                h.update(bytes.fromhex(pixels(folder / f'{x}_{y}.jpg',
                    (min(256, width - 256*x), min(256, height - 256*y)))))
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('baseline', 'candidate', 'project', 'output'):
        p.add_argument('--' + k, type=Path, required=True)
    p.add_argument('--formats', nargs='+', choices=('png', 'jpg', 'tiff', 'html', 'deepzoom'), default=['png', 'jpg', 'tiff', 'html'])
    p.add_argument('--threads', type=int, default=8)
    p.add_argument('--warmups', type=int, default=2)
    p.add_argument('--rounds', type=int, default=8)
    a = p.parse_args()
    if len(set(a.formats)) != len(a.formats) or a.rounds < 1 or a.warmups < 0:
        p.error('invalid experiment')
    manifest = (a.project / 'project.json' if a.project.is_dir() else a.project).resolve()
    layout = json.loads(manifest.read_bytes())
    dims = layout['grid']['columns'] * 180, layout['grid']['rows'] * 320
    if set(a.formats) & {'png', 'jpg', 'tiff'} and dims[0] * dims[1] > Image.MAX_IMAGE_PIXELS:
        p.error('use a smaller fixed layout for bounded, decoded-pixel validation')
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    bins = [getattr(a, name).resolve() for name in ('baseline', 'candidate')]
    data = dict(reference='baseline', variants=[dict(id=n, sha256=digest(b)) for n,b in zip(('baseline','candidate'),bins)],
                cases=[dict(id=f) for f in a.formats], samples=[], complete=False,
                project_sha256=digest(manifest), width=dims[0], height=dims[1], threads=a.threads,
                exploratory=a.rounds < 8 or a.warmups < 2)
    refs = {}; rng = random.Random(29)
    orders = {f: balanced_orders(2,a.rounds,rng) for f in a.formats}
    for phase, rounds in [('warmup', a.warmups), ('measured', a.rounds)]:
        for r in range(rounds):
            for f in (a.formats if r % 2 == 0 else reversed(a.formats)):
                for i in orders[f][r % a.rounds]:
                    name = data['variants'][i]['id']; tag = f'{phase}-{r}-{f}-{name}'
                    directory = f in ('html', 'deepzoom')
                    path = out / (name + ('-' if directory else '.') + f)
                    args = ['--mode', f] if directory else ['--write-mode','stream']
                    cmd = [bins[i], 'render', manifest, '-o', path, '--threads', str(a.threads),
                           '--output-tile', '180x320', '--overwrite', '--json', *args]
                    with (out / (tag + '.log')).open('wb') as log:
                        start = time.perf_counter(); resp = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=log, check=True)
                        wall = 1000 * (time.perf_counter() - start)
                    result = json.loads(resp.stdout); save(out / (tag + '.json'),result)
                    if not result['ok'] or result['warningCount']:
                        raise RuntimeError(tag + ': invalid render')
                    if f == 'html':
                        subprocess.run([bins[i],'project','check',path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,check=True)
                        artifact = path / 'project.json'
                        signature = digest(artifact)
                        size = sum(v.stat().st_size for v in path.rglob('*') if v.is_file())
                    elif f == 'deepzoom':
                        signature = deepzoom_pixels(path, dims)
                        size = sum(v.stat().st_size for v in path.rglob('*') if v.is_file())
                    else:
                        signature, size = pixels(path,dims), path.stat().st_size
                    if signature != refs.setdefault(f, signature):
                        raise ValueError(tag + ': rendered pixels or verified manifest changed')
                    data['samples'].append(dict(case=f, round=r, variant=name, phase=phase, content_sha256=signature,
                                                bytes=size,metrics={'processWallMs':wall}))
                    save(out / 'report.json',data); print(tag, round(wall), size, flush=True)
    if data['project_sha256'] != digest(manifest):
        raise ValueError('source manifest changed')
    data.update(complete=True, summary=summarize(data)); save(out / 'report.json', data)


if __name__ == '__main__':
    main()
