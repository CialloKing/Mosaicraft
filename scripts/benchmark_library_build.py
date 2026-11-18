"""Time actual build and add, verifying logical rows and per-image features after each task."""
import argparse
import hashlib
import json
import random
import sqlite3
import subprocess
import tempfile
import time
from contextlib import closing
from pathlib import Path

from benchmark_backends import balanced_orders, save, summarize
from study_paths import digest, readonly_database


def logical_rows(root):
    h = hashlib.sha256()
    with readonly_database(root/'mosaicraft.db') as db:
        for row in db.execute('SELECT id,file_path,file_hash,avg_l,avg_a,avg_b,edge_density,grid_lab,feature_version,use_count,tiny_path,lbp_path FROM images ORDER BY id'):
            for v in row[:-2]:
                b = v if isinstance(v,bytes) else repr(v).encode()
                h.update(len(b).to_bytes(4,'little')); h.update(b)
            for rel in row[-2:]:
                h.update(bytes.fromhex(digest(root/rel)))
        count, = db.execute('SELECT COUNT(*) FROM images').fetchone()
    return h.hexdigest(), count


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('baseline','candidate','inputs','append','output'):
        p.add_argument('--'+k,type=Path,required=True)
    p.add_argument('--threads',type=int,default=8)
    p.add_argument('--rounds',type=int,default=8); p.add_argument('--warmups',type=int,default=2)
    a = p.parse_args()
    if a.rounds < 1 or a.warmups < 0:
        p.error('invalid sample counts')
    out = a.output.resolve(); out.mkdir(parents=True,exist_ok=False)
    bins = [a.baseline.resolve(),a.candidate.resolve()]; inputs = [a.inputs.resolve(),a.append.resolve()]
    if not all(x.is_dir() for x in inputs):
        p.error('supply two separate nonempty source directories')
    variants = [dict(id=k,sha256=digest(b)) for k,b in zip(('baseline','candidate'),bins)]
    data = dict(reference='baseline', variants=variants,cases=[dict(id=k) for k in ('build','add')],samples=[],
                complete=False,threads=a.threads,exploratory=a.rounds<8 or a.warmups<2,
                sources=[{x.name:digest(x) for x in sorted(d.iterdir()) if x.is_file()} for d in inputs])
    reference = {}; orders = balanced_orders(2,a.rounds,random.Random(29))
    for phase,n in [('warmup',a.warmups),('measured',a.rounds)]:
        for r in range(n):
            for i in orders[r%a.rounds]:
                name = variants[i]['id']; previous = 0
                # 每对有独立新图库；追加沿用该构建的事务和身份，不向原图目录写入。
                with tempfile.TemporaryDirectory(dir=out) as work:
                    root = Path(work)/'library'
                    for verb, flag, src in zip(('build','add'),('-o','-l'),inputs):
                        tag = f'{phase}-{r}-{verb}-{name}'
                        cmd = [bins[i],'library',verb,src,flag,root,'--threads',str(a.threads),'--backend','cpu','--json']
                        with (out/(tag+'.log')).open('wb') as log:
                            start=time.perf_counter(); result=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=log,check=True)
                            wall=(time.perf_counter()-start)*1000
                        res=json.loads(result.stdout); save(out/(tag+'.json'),res)
                        sig,count=logical_rows(root)
                        if not res['ok'] or res['warningCount'] or count <= previous or reference.setdefault(verb,sig) != sig:
                            raise ValueError(tag+': incomplete or inequivalent library')
                        rows=count-previous; previous=count
                        data['samples'].append(dict(phase=phase,round=r,variant=name,case=verb,logical_sha256=sig,
                            rows=rows,images_per_second=rows*1000/wall,metrics=dict(processWallMs=wall)))
                        save(out/'report.json',data); print(tag,round(wall),rows,flush=True)
    data.update(complete=True,summary=summarize(data)); save(out/'report.json',data)

if __name__=='__main__':
    main()
