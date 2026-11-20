"""Process-level rejection and relocation checks; standard library only."""
import argparse
import concurrent.futures
from contextlib import closing
import hashlib
import json
from pathlib import Path
import sqlite3
import struct
import subprocess
import tempfile
import zlib


def chunk(kind, value):
    return struct.pack('>I', len(value)) + kind + value + struct.pack('>I', zlib.crc32(kind + value))


def png(path, color):
    pixels = b''.join(b'\0' + bytes(color) * 32 for _ in range(32))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 32, 32, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))


def check_usage(root, run):
    source = root / 'usage-source'
    source.mkdir()
    png(source / 'blue.png', (0, 0, 255))
    run(['library', 'build', source, '-o', 'usage-library', '--size', '32x32', '--backend', 'cpu'])
    database = root / 'usage-library/mosaicraft.db'
    with closing(sqlite3.connect(database)) as db, db:
        old_id, = db.execute('SELECT id FROM images').fetchone()
        db.execute('INSERT INTO usage_stats(image_id,total_runs,total_tiles) VALUES(?,7,30)', (old_id,))
        db.execute("INSERT INTO target_runs(target_hash,run_count) VALUES('legacy',7)")
    legacy = json.loads(run(['library', 'usage', database, '--json']))['data']['top'][0]
    assert legacy['runs'] == 0 and legacy['tiles'] == 30
    red, green, equivalent = (root / f'{name}.png' for name in ('red', 'green', 'equivalent'))
    png(red, (255, 0, 0))
    png(green, (0, 255, 0))
    data = red.read_bytes()
    equivalent.write_bytes(data[:33] + chunk(b'tEXt', b'Description\0equivalent target') + data[33:])

    def create(target, name):
        run(['create', target, '-l', database, '-o', name + '.png', '--backend', 'cpu', '--seed', '21',
             '--match-tile', '32x32', '--output-tile', '16x16', '--l-range', '255'])

    create(red, 'red-output')
    create(green, 'green-output')
    create(equivalent, 'equivalent-output')
    png(source / 'red.png', (255, 0, 0))
    png(source / 'black.png', (0, 0, 0))
    run(['library', 'add', source, '-l', database, '--backend', 'cpu'])
    create(red, 'red-with-new-material')
    with closing(sqlite3.connect(database)) as db:
        assert db.execute('SELECT run_count FROM target_runs ORDER BY run_count').fetchall() == [(1,), (3,), (7,)]
        assert db.execute('SELECT total_runs,total_tiles FROM usage_stats WHERE image_id=?', (old_id,)).fetchone() == (7, 33)
        assert db.execute('SELECT count(*) FROM image_target_runs GROUP BY image_id ORDER BY count(*)').fetchall() == [(1,), (2,)]
    run(['library', 'export', database, '-o', 'usage-export', '--selection', 'all'])
    names = {p.name for p in (root / 'usage-export').iterdir()}
    assert len(names) == 3 and all(any(part in name for name in names) for part in
                                   ('_2runs_33tiles_', '_1runs_1tiles_', '_0runs_0tiles_'))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', type=Path, required=True)
    executable = parser.parse_args().cli.resolve()
    with tempfile.TemporaryDirectory(prefix='mosaicraft-portable-') as folder:
        root = Path(folder)
        old = root / 'old.db'
        writer = sqlite3.connect(old)
        writer.executescript('PRAGMA journal_mode=WAL;CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT);'
            "INSERT INTO meta VALUES('path_format','db-relative-v1');"
            'CREATE TABLE images(id INTEGER PRIMARY KEY,use_count INTEGER);INSERT INTO images VALUES(1,9);')
        writer.commit()
        before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (old, Path(str(old) + '-wal'))}

        def run(arguments, success=True, error=b'library_incompatible', cwd=root):
            result = subprocess.run([str(executable), *arguments], cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            assert (result.returncode == 0) == success, result.stdout.decode('utf8', errors='replace')
            if not success:
                assert error in result.stdout, result.stdout.decode('utf8', errors='replace')
            return result.stdout

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
            list(workers.map(lambda _: run(['library', 'report', str(old), '--json'], False), range(4)))
        assert before == {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (old, Path(str(old) + '-wal'))}
        assert writer.execute('PRAGMA user_version').fetchone()[0] == 0
        assert writer.execute('SELECT use_count FROM images').fetchone()[0] == 9
        writer.close()
        assert not (root / '.mosaicraft-backups').exists()

        # A palette PNG must stay indexed and keep transparency and metadata on both sides of IDAT.
        raw = (b'\0' + bytes(range(2)) * 16) * 32
        head = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 32, 32, 8, 3, 0, 0, 0)) +
                chunk(b'PLTE', bytes(range(6))) + chunk(b'tRNS', b'\0\xff'))
        tail = chunk(b'tEXt', b'Author\0Mosaicraft') + chunk(b'IEND', b'')
        data = zlib.compress(raw, 0)
        original = head + chunk(b'IDAT', data[:1]) + chunk(b'IDAT', data[1:]) + tail
        palette = root / 'palette'
        palette.mkdir()
        (palette / 'input.PNG').write_bytes(original)
        run(['library', 'normalize', 'palette', '-o', 'compact', '--size', '32x32'])
        packed = next((root / 'compact').iterdir()).read_bytes()
        assert len(packed) < len(original) and packed.startswith(head) and packed.endswith(tail)
        assert zlib.decompress(packed[len(head) + 8:-len(tail) - 4]) == raw
        run(['library', 'normalize', 'compact', '-o', 'again', '--size', '32x32'])
        assert next((root / 'again').iterdir()).read_bytes() == packed
        assert (palette / 'input.PNG').read_bytes() == original

        retry = ['library', 'build', 'not-ready', '-o', 'retry', '--size', '32x32', '--backend', 'cpu', '--json']
        run(retry, False, b'input_unreadable')
        assert not (root / 'retry/mosaicraft.db').exists()
        (root / 'not-ready').mkdir()
        bad = root / 'not-ready/material.png'
        bad.write_bytes(b'not a PNG')
        run(retry, False, b'input_unreadable')
        assert (root / 'retry/mosaicraft.db').is_file()
        png(bad, (1, 2, 3))
        run(retry)
        run(retry, False, b'library already exists')

        inplace = root / 'inplace'
        inplace.mkdir()
        original_path = inplace / 'original.png'
        png(original_path, (2, 3, 4))
        original_bytes = original_path.read_bytes()
        build_inplace = ['library', 'build', './inplace', '-o', 'inplace/', '--backend', 'cpu', '--size']
        run([*build_inplace, '16x24'], False, b'use a separate output directory')
        assert original_path.read_bytes() == original_bytes and not (inplace / 'mosaicraft.db').exists()
        run([*build_inplace, '32x32'])
        assert not original_path.exists() and (inplace / '000000.png').read_bytes() == original_bytes

        source = root / 'source'
        source.mkdir()
        for number, color in enumerate(((30, 60, 90), (100, 130, 160), (170, 200, 230))):
            png(source / f'{number}.png', color)
        library = root / 'library A'
        database = library / 'mosaicraft.db'
        run(['library', 'build', str(source), '-o', str(library), '--threads', '1', '--backend', 'cpu'])
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
            list(workers.map(lambda _: run(['library', 'report', str(database)]), range(4)))
        connection = sqlite3.connect(database)
        try:
            assert connection.execute('PRAGMA user_version').fetchone()[0] == 1
            assert all(not Path(p).is_absolute() for row in connection.execute('SELECT file_path,tiny_path,lbp_path FROM images') for p in row)
            material, old, digest, tiny, lbp = connection.execute(
                'SELECT id,file_path,file_hash,tiny_path,lbp_path FROM images ORDER BY id LIMIT 1').fetchone()
        finally:
            connection.close()

        for flags in ([], ['--apply']):
            run(['library', 'purge', 'typo.db', *flags, '--json'], False, b'library_unavailable')
            assert not (root / 'typo.db').exists()

        contracts = json.loads((Path(__file__).resolve().parents[1] / 'contracts.json').read_text(encoding='utf-8'))
        token = '1234'
        staging = library / ('.mosaicraft-color-order-' + token)
        staging.mkdir()
        staged = staging / f'{material}.stage'
        (library / old).rename(staged)
        journal = Path(str(database) + '.color-order.json')
        empty = root / 'empty'
        empty.mkdir()
        add = ['library', 'add', str(empty), '-l', str(database), '--backend', 'cpu']
        journal.write_text('{', encoding='utf-8')
        run(add, False, b'color reorder recovery failed')
        assert staged.is_file() and journal.read_text(encoding='utf-8') == '{'
        data = dict(version=contracts['journalVersion'], library_version=contracts['libraryVersion'],
                    feature_version=contracts['featureVersion'], token=token, database=database.name,
                    output='.', staging=staging.name, phase='staging', entries=[dict(
                        id=material, old_db_path=old, original=old, final='999999.png',
                        temporary=staged.relative_to(library).as_posix(), hash=digest, size=staged.stat().st_size)])
        journal.write_text(json.dumps(data), encoding='utf-8')
        protected = (database, journal, staged, library / tiny, library / lbp)
        before = [path.read_bytes() for path in protected]
        for flags in ([], ['--apply']):
            run(['library', 'purge', str(database), *flags], False, b'unfinished color reorder')
            assert [path.read_bytes() for path in protected] == before
        run(add)
        assert not journal.exists() and not staging.exists() and (library / old).read_bytes() == before[2]
        run(['library', 'add', str(empty), '-l', 'mosaicraft.db', '--backend', 'cpu'], cwd=library)

        # Include a character that cannot be represented by a Windows legacy code page.
        moved = root / '图库 𠮷 B'
        library.rename(moved)
        run(['library', 'report', str(moved)])
        run(['library', 'report', str(moved), '--check'])
        assert not (moved / '.mosaicraft-backups').exists()
        check_usage(root, run)
        print('four readers reject old WAL library unchanged; new library concurrency and relocation: passed')


if __name__ == '__main__':
    main()
