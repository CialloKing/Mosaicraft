"""Process-level rejection and relocation checks; standard library only."""
import argparse
import concurrent.futures
import hashlib
from pathlib import Path
import sqlite3
import struct
import subprocess
import tempfile
import zlib


def png(path, color):
    def chunk(kind, value):
        return struct.pack('>I', len(value)) + kind + value + struct.pack('>I', zlib.crc32(kind + value))
    pixels = b''.join(b'\0' + bytes(color) * 32 for _ in range(32))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 32, 32, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))


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

        def run(arguments, success=True):
            result = subprocess.run([str(executable), *arguments], cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            assert (result.returncode == 0) == success, result.stdout.decode('utf8', errors='replace')
            if not success:
                assert b'library_incompatible' in result.stdout

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
            list(workers.map(lambda _: run(['library', 'report', str(old), '--json'], False), range(4)))
        assert before == {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (old, Path(str(old) + '-wal'))}
        assert writer.execute('PRAGMA user_version').fetchone()[0] == 0
        assert writer.execute('SELECT use_count FROM images').fetchone()[0] == 9
        writer.close()
        assert not (root / '.mosaicraft-backups').exists()

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
        finally:
            connection.close()
        # Include a character that cannot be represented by a Windows legacy code page.
        moved = root / '图库 𠮷 B'
        library.rename(moved)
        run(['library', 'report', str(moved)])
        run(['library', 'report', str(moved), '--check'])
        assert not (moved / '.mosaicraft-backups').exists()
        print('four readers reject old WAL library unchanged; new library concurrency and relocation: passed')


if __name__ == '__main__':
    main()
