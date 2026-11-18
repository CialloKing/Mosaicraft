"""Compare fast/compatible selections, using a prepared private library and the FP64 evaluator."""
import argparse
import json
import sqlite3
import statistics
import uuid
from contextlib import closing
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps
from benchmark_backends import run, save
from study_paths import digest, readonly_database


def targets(inputs, folder):
    size = (180, 320)
    cases = []
    for name in ('photo', 'screen', 'gradient', 'texture', 'edge', 'scarce'):
        if name in ('photo', 'screen'):
            with Image.open(inputs / ('target3.jpg' if name == 'photo' else '屏幕测试图.png')) as original:
                image = ImageOps.fit(original.convert('RGB'), size)
        else:
            image = Image.new('RGB', size)
            if name == 'edge':
                ImageDraw.Draw(image).polygon([(0, 0), (180, 320), (0, 320)], fill='white')
            elif name == 'scarce':
                # 黑色格在真实库的极暗端仅有少许合法候选；中间色带保证不是全空任务。
                ImageDraw.Draw(image).rectangle((72, 0, 107, 319), fill=(110, 110, 110))
            else:
                image.putdata([(v, v, v) for y in range(size[1]) for x in range(size[0])
                               for v in [round(255*y/319) if name == 'gradient' else 45+160*((x//3+y//4) % 2)]])
        path = folder / (name + '.png')
        image.save(path)
        cases.append(dict(id=name, input=str(path), width=size[0], height=size[1], sha256=digest(path)))
    return cases


def assess(rows, groups):
    result = []
    for key, modes in groups.items():
        def med(mode, field):
            values = [rows[tag] for tag in modes[mode]]
            return statistics.median(v['neighborhood']['3']['same_pair_rate'] if field == 'repeat' else v[field]
                                     for v in values)
        fields = ('mean', 'p90', 'p99', 'repeat', 'unique_images')
        old, new = ({k: med(mode, k) for k in fields} for mode in ('compatible', 'fast'))
        missing = all(rows[b]['missing'] <= rows[a]['missing'] for a,b in zip(modes['compatible'], modes['fast']))
        passed = missing and all(new[k] <= 1.03*old[k]+1e-6 for k in fields[:3]) and (
            new['repeat'] <= old['repeat'] + 0.02 and new['unique_images'] >= 0.90*old['unique_images'])
        result.append(dict(group=key, passed=passed, compatible=old, fast=new, coverage_ok=missing))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ('prepared-run','match','quality','inputs','output'):
        parser.add_argument('--'+field, required=True, type=Path)
    parser.add_argument('--device', action='append', required=True, help='cpu or a discovered cuda/vulkan/metal device ID')
    args = parser.parse_args()
    previous = json.loads((args.prepared_run / 'report.json').read_bytes())
    snapshot = Path(previous.get('library_snapshot', args.prepared_run / 'library/snapshot.db')).resolve()
    if not previous['complete'] or digest(snapshot) != previous['snapshot_sha256']:
        raise ValueError('need an unchanged, completed private snapshot')
    folder = args.output.resolve()
    folder.mkdir(parents=True, exist_ok=False)
    database = snapshot.with_name('quality-' + uuid.uuid4().hex + '.db')
    with readonly_database(snapshot) as src, closing(sqlite3.connect(database)) as dest:
        src.backup(dest)
    study = dict(snapshot=str(database), oracle=False, cases=targets(args.inputs,folder), runs=[])
    groups = {}
    for device in args.device:
        backend = device.split(':')[0]
        for case in study['cases']:
            for top in (1,10):
                group = f"{device.replace(':','-')}-{case['id']}-top{top}"
                groups[group] = {mode: [] for mode in ('compatible','fast')}
                for seed in (42, 20261110, 20261111):
                    for mode in ('compatible','fast'):
                        tag = f'{group}-{seed}-{mode}'
                        output = folder / (tag+'.png')
                        request = dict(inputPath=case['input'], dbPath=str(database), outputPath=str(output),
                            outW=case['width'],outH=case['height'], tileW=9,tileH=16,nativeTileW=9,nativeTileH=16,
                            topNRandom=top, candidates=150,lRange=21 if case['id']=='scarce' else 255,
                            seed=seed, backend=backend,device=device,scoreMode=mode,threads=8,
                            resourceMonitor=False,analyze=False,generatePreview=False,colorAdjust=False,format='png')
                        req, summary = folder/(tag+'.request.json'), folder/(tag+'.summary.json')
                        save(req, request)
                        run([args.match,req,'--match-only','--seed',seed,'--no-detail','--summary',summary],folder/(tag+'.log'))
                        actual = json.loads(summary.read_bytes())
                        scorer = 'cpu' if mode=='compatible' and backend in ('metal','vulkan') else backend
                        if actual['failed'] or actual['warnings'] or actual['device']!=device or scorer not in actual['score_backend'] or actual['score_mode']!=mode:
                            raise ValueError(f'{tag}: unexpected execution path')
                        selection = Path(str(output)+'.indices.bin')
                        study['runs'].append(dict(id=tag,case=case['id'],selection=str(selection),selection_sha256=digest(selection)))
                        groups[group][mode].append(tag)
                print(group,flush=True)
    save(folder/'study.json',study)
    run([args.quality,folder/'study.json',folder/'evaluation.json'],folder/'evaluation.log')
    evaluation=json.loads((folder/'evaluation.json').read_bytes())
    assert evaluation['complete'] and len(evaluation['runs'])==len(study['runs'])
    groups=assess({r['id']:r['quality'] for r in evaluation['runs']},groups)
    if digest(snapshot)!=previous['snapshot_sha256']:
        raise RuntimeError('private source snapshot changed')
    save(folder/'gate.json',dict(passed=all(row['passed'] for row in groups), groups=groups,
        snapshot_sha256=digest(snapshot),match_sha256=digest(args.match),quality_sha256=digest(args.quality)))
    return 0 if all(row['passed'] for row in groups) else 2


if __name__ == '__main__':
    raise SystemExit(main())
