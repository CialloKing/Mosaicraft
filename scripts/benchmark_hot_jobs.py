"""Time consecutive formal Web UI jobs with current usage statistics (not independent cold CLI runs)."""
import argparse
import json
import os
import random
import socket
import sqlite3
import subprocess
import time
import uuid
from contextlib import closing
from pathlib import Path
from urllib.request import ProxyHandler, Request, build_opener

from benchmark_backends import balanced_orders, save, summarize
from study_paths import digest, readonly_database


def session(a,exe,src,source,out):
    db=src.with_name('session-'+uuid.uuid4().hex+'.db'); target=a.input.resolve()
    data=dict(complete=False,snapshot_sha256=source,binary_sha256=digest(exe),input_sha256=digest(target),samples=[])
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]
    # Match the server's localhost listener (which can be IPv6-only); never proxy a private job.
    client=build_opener(ProxyHandler({}))
    def api(path,body=None):
        req=Request(f'http://localhost:{port}'+path, data=None if body is None else json.dumps(body).encode(),
                    headers={'Content-Type':'application/json'})
        with client.open(req,timeout=20) as res:
            value=json.load(res)
        if not value.get('ok'):
            raise RuntimeError(value)
        return value
    def usage():
        with readonly_database(db) as c:
            return c.execute('SELECT coalesce(sum(total_tiles),0) FROM usage_stats').fetchone()[0]
    try:
        with readonly_database(src) as original, closing(sqlite3.connect(db)) as dest:
            original.backup(dest)
        with (out/'server.log').open('wb') as log:
            proc=subprocess.Popen([exe,str(port),'--no-open'],cwd=out,stdout=log,stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
            try:
                for _ in range(100):
                    if proc.poll() is not None:
                        raise RuntimeError('server exited')
                    try:
                        api('/api/info');break
                    except OSError:
                        time.sleep(.1)
                else:
                    raise TimeoutError('server startup')
                before=usage()
                for i in range(a.jobs):
                    image=out/f'task-{i}.png'
                    body=dict(inputPath=str(target),dbPath=str(db),outputPath=str(image),format='png',backend=a.backend,
                        tileW=9,tileH=16,nativeTileW=9,nativeTileH=16,outW=270,outH=480,threads=8,candidates=150,
                        seed=42,topNRandom=10,lRange=255,generatePreview=False,scoreMode='fast',resourceMonitor=False)
                    start=time.perf_counter(); job=api('/api/jobs/mosaic',body)['job']
                    while job['state'] not in ('succeeded','failed','canceled'):
                        if time.perf_counter()-start>300:
                            raise TimeoutError(job['id'])
                        time.sleep(.025); job=api('/api/jobs/'+job['id'])['job']
                    wall=1000*(time.perf_counter()-start); save(out/f'task-{i}.json',job); actual=job['runtime']
                    if (job['state']!='succeeded' or job['warningCount'] or actual['matched']!=900 or actual['failed'] or
                            actual['device'].split(':')[0]!=a.backend or usage()!=before+(i+1)*900):
                        raise ValueError('job failed or usage was not committed')
                    data['samples'].append(dict(task=i+1,wall_ms=wall,metrics=actual['metrics'],png_sha256=digest(image)))
                    save(out/'report.json',data); print(i+1,round(wall),actual['metrics'].get('libraryFeatureRowsReused'),flush=True)
            finally:
                proc.terminate(); proc.wait(timeout=30)
        if digest(src)!=source:
            raise RuntimeError('snapshot changed')
        data.update(complete=True);save(out/'report.json',data)
        return data['samples']
    finally:
        for suffix in ('','-shm','-wal','-journal','.library.lock'):
            Path(str(db)+suffix).unlink(missing_ok=True)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for k in ('server','prepared','input','output'):
        p.add_argument('--'+k,type=Path,required=True)
    p.add_argument('--jobs',type=int,default=10)
    p.add_argument('--backend',choices=('cpu','cuda'),default='cpu')
    p.add_argument('--candidate',type=Path,help='pair complete sessions using a second server')
    p.add_argument('--rounds',type=int,default=8); p.add_argument('--warmups',type=int,default=2)
    a=p.parse_args(); src=(a.prepared/'library/snapshot.db').resolve()
    source=digest(src); manifest=json.loads((a.prepared/'report.json').read_bytes())
    if a.rounds<1 or a.warmups<0:
        p.error('invalid sample counts')
    if not manifest['complete'] or manifest.get('snapshot_sha256',source)!=source or a.jobs<2:
        p.error('requires a prepared private library and at least two tasks')
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    if not a.candidate:
        session(a,a.server.resolve(),src,source,out)
        return
    bins=[a.server.resolve(),a.candidate.resolve()]
    variants=[dict(id=name,sha256=digest(exe)) for name,exe in zip(('baseline','candidate'),bins)]
    keys=('first','second','session_total')
    report=dict(reference='baseline',variants=variants,cases=[dict(id=k) for k in keys],
                backend=a.backend,jobs=a.jobs,snapshot_sha256=source,input_sha256=digest(a.input),
                exploratory=a.rounds<8 or a.warmups<2,complete=False,samples=[])
    orders=balanced_orders(2,a.rounds,random.Random(29)); expected=None
    for phase,count in [('warmup',a.warmups),('measured',a.rounds)]:
        for r in range(count):
            for i in orders[r%a.rounds]:
                name=variants[i]['id']; folder=out/f'{phase}-{r}-{name}'; folder.mkdir()
                jobs=session(a,bins[i],src,source,folder)
                hashes=[job['png_sha256'] for job in jobs]
                if expected is None:
                    expected=hashes
                # Usage deliberately advances between jobs; compare the same ordinal, not job one ten times.
                if a.backend=='cpu' and hashes!=expected:
                    raise ValueError(f'{folder}: different selections/output across CPU sessions')
                for k,ms in zip(keys,(jobs[0]['wall_ms'],jobs[1]['wall_ms'],sum(j['wall_ms'] for j in jobs))):
                    report['samples'].append(dict(phase=phase,round=r,case=k,variant=name,metrics=dict(processWallMs=ms)))
                save(out/'report.json',report)
    report.update(complete=True,summary=summarize(report));save(out/'report.json',report)


if __name__=='__main__':
    main()
