"""Portable checkpoint paths, independent of the caller's working directory.

Temporary C++ request files contain resolved paths; saved checkpoints contain
relative references and external library/tool bindings supplied by the caller.
"""
import hashlib
import json
import os
import re
from pathlib import Path, PureWindowsPath
import shutil
import sqlite3
from contextlib import contextmanager, closing

REPO = Path(__file__).resolve().parents[1]
FORMAT = "db-relative-v1"
PATH_KEYS = {"path", "snapshot", "source_database", "baseline_study", "input", "output", "db",
             "inputPath", "outputPath", "dbPath", "selection", "diagnostics", "database"}
_tools = {}
_database = None


def configure(args):
    global _tools, _database
    for name in ("match", "quality", "cli", "exe", "baseline_exe", "inputs", "baseline_study", "output_dir", "database"):
        value = getattr(args, name, None)
        if value is not None:
            setattr(args, name, Path(value).resolve())
    _tools = {name: str(getattr(args, name)) for name in ("match", "quality", "cli") if getattr(args, name, None)}
    _database = getattr(args, "database", None)


def bind_database(path):
    global _database
    _database = Path(path).resolve()


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def absolute(value):
    return Path(value).is_absolute() or PureWindowsPath(value).is_absolute()


def _convert(value, parent, writing, key=""):
    if isinstance(value, dict):
        return {k: _convert(v, parent, writing, k) for k, v in value.items()}
    if isinstance(value, list):
        return [_convert(v, parent, writing, key) for v in value]
    if not isinstance(value, str) or not value or key not in PATH_KEYS or re.fullmatch(r"[a-fA-F0-9]{40,64}", value):
        return value
    if writing:
        if not absolute(value):
            return value
        for name, tool in _tools.items():
            if value == tool:
                return "@tool/" + name
        if _database is not None and Path(value) == _database:
            return "@library/database"
        try:
            return Path(os.path.relpath(value, parent)).as_posix()
        except ValueError:
            raise ValueError(f"Cannot persist cross-volume reference {key}; supply a local resource binding") from None
    if value.startswith("@tool/"):
        return _tools[value.split("/", 1)[1]]
    if value == "@library/database":
        if _database is None:
            raise ValueError("This study requires --database to bind the local library")
        return str(_database)
    return value if absolute(value) else str((parent / value).resolve())


def save(path, value):
    path = Path(path)
    # These two files are consumed immediately by C++, relative to its process.
    if path.name not in ("request.json", "study.json", "public-results.json"):
        value = _convert(value, path.resolve().parent, True)
        if isinstance(value, dict):
            value = dict(value, checkpoint_paths=2)
    temporary = Path(str(path) + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding="utf-8")
    temporary.replace(path)


def load(path):
    path = Path(path)
    value = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(value, dict):
        if value.get("paths_redacted"):
            raise ValueError("A public redacted report is not a runtime checkpoint")
        value.pop("checkpoint_paths", None)
    return _convert(value, path.resolve().parent, False)


@contextmanager
def readonly_database(path):
    connection = sqlite3.connect(Path(path).resolve().as_uri() + "?mode=ro", uri=True)
    try:
        yield connection
    finally:
        connection.close()


def path_mode(connection):
    row = connection.execute("SELECT value FROM meta WHERE key='path_format'").fetchone()
    mode = row[0] if row else ""
    if mode not in ("", FORMAT):
        raise ValueError(f"Unknown library path format: {mode}")
    return mode


def resource(database, value, mode):
    if not value:
        return None
    if mode == FORMAT:
        path = PureWindowsPath(value)
        if absolute(value) or "\\" in value or ".." in path.parts:
            raise ValueError("Invalid portable library path")
        return Path(database).resolve().parent / value
    return Path(value).resolve()


def source_inventory(database):
    summary = hashlib.sha256()
    with readonly_database(database) as connection:
        mode = path_mode(connection)
        for row in connection.execute("SELECT id,file_path,tiny_path,hist_path FROM images ORDER BY id"):
            summary.update(str(row[0]).encode())
            for value in row[1:]:
                path = resource(database, value, mode)
                summary.update((digest(path) if path else "missing").encode())
    return summary.hexdigest()


def private_library(source, root, images):
    """Copy the index without modifying source payloads; run.db shares this root."""
    root = Path(root).resolve()
    root.mkdir(parents=True, exist_ok=True)
    snapshot = root / "snapshot.db"
    with readonly_database(source) as original, closing(sqlite3.connect(snapshot)) as target:
        original.backup(target)
        mode = path_mode(original)
        rows = original.execute("SELECT id,file_path,tiny_path,hist_path FROM images ORDER BY id").fetchall()
        target.execute("BEGIN IMMEDIATE")
        target.execute("UPDATE images SET file_path='.__study_stage__/' || id")
        for image_id, image, tiny, histogram in rows:
            new_paths = []
            for value, relative, include in ((image, f"images/id_{image_id}{Path(image).suffix}", images),
                                            (tiny, f"features/id_{image_id}.tiny", True),
                                            (histogram, f"features/id_{image_id}.hist", True)):
                if value and include:
                    old = resource(source, value, mode)
                    new = root / relative
                    new.parent.mkdir(parents=True, exist_ok=True)
                    if not new.exists():
                        try:
                            new.hardlink_to(old)
                        except OSError:
                            shutil.copy2(old, new)
                new_paths.append(relative if value else None)
            target.execute("UPDATE images SET file_path=?,tiny_path=?,hist_path=? WHERE id=?", (*new_paths, image_id))
        target.execute("INSERT OR REPLACE INTO meta VALUES('path_format',?)", (FORMAT,))
        target.commit()
    return snapshot, len(rows)
