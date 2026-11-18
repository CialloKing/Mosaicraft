"""Time consecutive formal Web UI jobs with current usage statistics (not independent cold CLI runs)."""
import argparse
import json
import os
import socket
import sqlite3
import subprocess
import time
import uuid
from contextlib import closing
from pathlib import Path
from urllib.request import ProxyHandler, Request, build_opener

from benchmark_backends import save
from study_paths import digest, readonly_database


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for k in ('server','prepared','input','output'):
        p.add_argument('--'+k,type=Path,required=True)
    p.add_argument('--jobs',type=int,default=10)
    p.add_argument('--backend',choices=('cpu','cuda'),default='cpu')
    a=p.parse_args(); exe=a.server.resolve(); src=(a.prepared/'library/snapshot.db').resolve()
    source=digest(src); manifest=json.loads((a.prepared/'report.json').read_bytes())
    if not manifest['complete'] or manifest.get('snapshot_sha256',source)!=source or a.jobs<2:
        p.error('requires a prepared private library and at least two tasks')
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
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
    finally:
        for suffix in ('','-shm','-wal','-journal','.library.lock'):
            Path(str(db)+suffix).unlink(missing_ok=True)

if __name__=='__main__':
    main()
