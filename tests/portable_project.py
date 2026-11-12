"""Portable project end-to-end checks using small generated images only."""
import argparse
from contextlib import closing
import hashlib
import json
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import tempfile
import zipfile

from portable_library import png


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', type=Path, required=True)
    cli = parser.parse_args().cli.resolve()
    with tempfile.TemporaryDirectory(prefix='mosaicraft-project-') as folder:
        root = Path(folder)

        def run(*args, ok=True):
            process = subprocess.run([str(cli), *map(str, args), '--json'], cwd=root,
                                     capture_output=True, timeout=90)
            result = json.loads(process.stdout)
            assert bool(result['ok']) == ok and (process.returncode == 0) == ok, result
            assert all(c >= 32 or c in (9, 10, 13) for c in process.stdout), process.stdout
            return result

        commands = [('create',), ('render',), ('inspect',)] + [
            ('library', name) for name in ('build', 'add', 'report', 'usage', 'export', 'purge', 'normalize')
        ] + [('project', name) for name in ('info', 'check', 'pack')]
        for command in commands:
            for flag in ('--help', '--help-all'):
                help_result = subprocess.run([str(cli), *command, flag], capture_output=True, timeout=10)
                assert help_result.returncode == 0 and b'Usage:' in help_result.stdout
                assert b'\x1b' not in help_result.stdout + help_result.stderr
        for old in ('mosaic', 'build', 'db-stats', 'db-health', 'db-usage', 'db-purge'):
            run(old, ok=False)

        photos = root / 'photos'
        photos.mkdir()
        for i in range(10):
            png(photos / f'{i}.png', (20 + i * 20, 30 + i * 16, 40 + i * 12))
        png(root / 'target.png', (100, 110, 120))
        run('library', 'build', photos, '-o', 'library', '--size', '18x32', '--backend', 'cpu')
        run('library', 'build', photos, '-o', 'library', ok=False)
        run('create', 'target.png', '-l', 'library', '-o', 'artwork', '--mode', 'html',
            '--backend', 'cpu', '--seed', 42, '--l-range', 255, '--target-blend', .2)
        manifest = json.loads((root / 'artwork/project.json').read_text())
        assert 'version' not in manifest
        assert manifest['appearance']['targetBlend'] == .2
        assert not any(Path(item['path']).is_absolute() for item in manifest['resources'])
        assert len(manifest['assets']) <= 10
        assert (root / 'artwork/target.png').is_file()
        assert not (root / 'artwork/overview.jpg').exists()
        assert not (root / 'artwork/thumbnails').exists()
        with closing(sqlite3.connect(root / 'library/mosaicraft.db')) as db:
            names = {Path(p).name for p, in db.execute('SELECT file_path FROM images')}
        assert all(Path(a['path']).name in names for a in manifest['assets'])
        assert len(list((root / 'artwork').rglob('*.*'))) == len(manifest['assets']) + 3
        run('project', 'check', 'artwork/index.html', '--deep')
        # Distinct content with the same name must survive without renaming; identical bytes copy once.
        fixture = root / 'same-names'
        fixture.mkdir()
        special = '素材 # % 𠮷.PNG'
        collision = json.loads(json.dumps(manifest))
        collision.update(grid={'columns': 2, 'rows': 2, 'matchTile': [16, 16]},
                         assets=[], layout=[0, 1, 2, 1], resources=[])
        for i, color in enumerate(((20, 50, 80), (180, 210, 240), (20, 50, 80))):
            name = f'{i}/{special}'
            (fixture / str(i)).mkdir()
            png(fixture / name, color)
            digest = hashlib.sha256((fixture / name).read_bytes()).hexdigest()
            collision['assets'].append(dict(path=name, sha256=digest, width=32, height=32))
        shutil.copyfile(root / 'target.png', fixture / 'target.png')
        for name in ['target.png', *[a['path'] for a in collision['assets']]]:
            collision['resources'].append(dict(path=name, bytes=(fixture / name).stat().st_size,
                sha256=hashlib.sha256((fixture / name).read_bytes()).hexdigest(), width=32, height=32))
        template = (root / 'artwork/index.html').read_text(encoding='utf-8')

        def save_fixture(directory, data):
            keys = ['grid', 'outputTile', 'layout', 'assets', 'appearance', 'target']
            snapshot = json.dumps({key: data[key] for key in keys}).replace('<', r'\u003c')
            html = re.sub(r'(<script id="mosaicraft-snapshot" type="application/json">).*?(</script>)',
                          lambda match: match[1] + snapshot + match[2], template, flags=re.S)
            (directory / 'index.html').write_text(html, encoding='utf-8')
            data['resources'] = [r for r in data['resources'] if r['path'] != 'index.html']
            data['resources'].append(dict(path='index.html', bytes=(directory / 'index.html').stat().st_size,
                sha256=hashlib.sha256((directory / 'index.html').read_bytes()).hexdigest(), width=0, height=0))
            (directory / 'project.json').write_text(json.dumps(data), encoding='utf-8')

        save_fixture(fixture, collision)
        run('project', 'check', fixture, '--deep')
        run('render', fixture, '-o', 'preserved', '--mode', 'html')
        preserved = json.loads((root / 'preserved/project.json').read_text(encoding='utf-8'))
        assert len(preserved['assets']) == 2
        assert all(Path(a['path']).name == special for a in preserved['assets'])
        assert preserved['layout'] == [0, 1, 0, 1]
        run('project', 'check', 'preserved', '--deep')
        quiet = subprocess.run([str(cli), 'project', 'info', 'artwork', '--quiet'], cwd=root,
                               capture_output=True, timeout=10)
        assert quiet.returncode == 0 and json.loads(quiet.stdout)['ok'] and not quiet.stderr
        assert 'version' not in json.loads(quiet.stdout)['project']
        for repeat in range(2):
            run('create', 'target.png', '-l', 'library', '-o', 'analyzed.png', '--analyze',
                '--backend', 'cpu', '--seed', 42, '--l-range', 255, *(['--overwrite'] if repeat else []))
        assert (root / 'analyzed_analysis/.mosaicraft-output.json').is_file()
        with closing(sqlite3.connect(root / 'library/mosaicraft.db')) as db:
            before_usage = list(db.execute('SELECT * FROM usage_stats ORDER BY image_id'))
        run('create', 'target.png', '-l', 'library', '-o', 'invalid', '--mode', 'html',
            '--brightness-jitter', .04, ok=False)
        assert not (root / 'invalid').exists()
        for command, flag in [('render', '--match-tile'), ('create', '--old-flag')]:
            run(command, 'artwork', '-o', 'result.png', flag, '9x16', ok=False)
        run('render', 'artwork/project.json', '-o', 'result.png', '--output-tile', '36x64',
            '--brightness-jitter', .04, '--target-blend', .3, '--threads', 1)
        run('render', 'artwork', '-o', 'again.png', '--output-tile', '36x64',
            '--brightness-jitter', .04, '--target-blend', .3, '--threads', 4)
        assert (root / 'result.png').read_bytes() == (root / 'again.png').read_bytes()
        with closing(sqlite3.connect(root / 'library/mosaicraft.db')) as db:
            assert before_usage == list(db.execute('SELECT * FROM usage_stats ORDER BY image_id'))
        # Actual codec suffix must also be protected, and a failed publish must not add usage.
        target_hash = hashlib.sha256((root / 'target.png').read_bytes()).hexdigest()
        run('create', 'target.png', '-l', 'library', '-o', 'target.jpg', '--format', 'png',
            '--overwrite', '--backend', 'cpu', '--seed', 42, '--l-range', 255, ok=False)
        assert target_hash == hashlib.sha256((root / 'target.png').read_bytes()).hexdigest()
        with closing(sqlite3.connect(root / 'library/mosaicraft.db')) as db:
            assert before_usage == list(db.execute('SELECT * FROM usage_stats ORDER BY image_id'))
        (root / 'library').rename(root / 'unavailable-library')
        (root / 'artwork').rename(root / '移动后的作品')
        project = root / '移动后的作品'
        run('render', project, '-o', 'relocated.png')
        run('project', 'pack', project, '-o', 'artwork.zip')
        with zipfile.ZipFile(root / 'artwork.zip') as archive:
            assert len({Path(name).parts[0] for name in archive.namelist()}) == 1
            assert archive.testzip() is None
            archive.extractall(root / 'unpacked')
        run('project', 'check', root / 'unpacked' / project.name, '--deep')
        run('render', root / 'unpacked' / project.name, '-o', 'unpacked.png')
        assert (root / 'unpacked.png').read_bytes() == (root / 'relocated.png').read_bytes()
        (project / 'unrelated.txt').write_text('keep')
        run('render', project, '-o', project, '--mode', 'html', '--overwrite', ok=False)
        # Both checking and rendering require the only stored copy of each source asset.
        source = project / manifest['assets'][0]['path']
        saved = source.read_bytes()
        source.unlink()
        run('project', 'check', project, ok=False)
        run('render', project, '-o', 'missing-source.png', ok=False)
        source.write_bytes(saved)
        pristine = (project / 'project.json').read_bytes()
        for mutate in (
            lambda m: m.update(type='unrelated-project'),
            lambda m: m.update(overview='target.png'),
            lambda m: m['layout'].__setitem__(0, 9999),
            lambda m: m['resources'][0].update(path='../outside.png'),
            lambda m: m['resources'][0].update(sha256='0' * 64),
            lambda m: m['assets'][0].update(width=1),
            lambda m: m.update(target='../outside.png'),
            lambda m: m['grid'].update(columns=2.5),
            lambda m: m.update(outputTile=[18.5, 32]),
        ):
            changed = json.loads(pristine)
            mutate(changed)
            (project / 'project.json').write_text(json.dumps(changed), encoding='utf-8')
            run('project', 'check', project, '--deep', ok=False)
        (project / 'project.json').write_bytes(pristine)
        asset = project / manifest['assets'][0]['path']
        original = asset.read_bytes()
        asset.write_bytes(original[:20])
        run('render', project, '-o', 'truncated.png', ok=False)
        asset.write_bytes(original)
        # Existing image remains intact if replacement encoding fails.
        before = (root / 'result.png').read_bytes()
        run('render', project, '-o', 'result.png', '--mode', 'html', '--overwrite', ok=False)
        assert (root / 'result.png').read_bytes() == before
        for mode in ('tiles', 'deepzoom', 'html'):
            run('render', project, '-o', mode, '--mode', mode)
            assert (root / mode).is_dir()
            if mode == 'html':
                run('project', 'check', mode, '--deep')
            run('render', project, '-o', mode, '--mode', mode, '--overwrite')
            (root / mode / 'keep.txt').write_text('unrelated')
            run('render', project, '-o', mode, '--mode', mode, '--overwrite', ok=False)
            assert (root / mode / 'keep.txt').read_text() == 'unrelated'
        print('portable HTML, relocation, deterministic rendering, ZIP64 and failure boundaries passed')


if __name__ == '__main__':
    main()
