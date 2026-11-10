"""Portable checkpoint and public-evidence contracts; standard library only."""
import argparse
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from study_paths import configure, save, load, private_library, readonly_database, resource
from public_paths import public_report


class StudyPaths(unittest.TestCase):
    def test_move_and_rebind_preserve_content_identity(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            first = root / "A"
            first.mkdir()
            args = argparse.Namespace(database=root / "library/db", match=root / "tools/match")
            configure(args)
            save(first / "manifest.json", dict(snapshot=str(first / "snapshot.db"),
                 source_database=str(args.database), executable=dict(path=str(args.match)),
                 identity=dict(snapshot="a" * 64)))
            stored = json.loads((first / "manifest.json").read_text(encoding="utf8"))
            self.assertEqual(stored["snapshot"], "snapshot.db")
            moved = root / "B"
            first.rename(moved)
            args.match = root / "other-tools/match"
            configure(args)
            result = load(moved / "manifest.json")
            self.assertEqual(result["snapshot"], str(moved / "snapshot.db"))
            self.assertEqual(result["executable"]["path"], str(args.match))
            self.assertEqual(result["identity"]["snapshot"], "a" * 64)

    def test_public_data_keeps_measurements_and_hashes(self):
        source = dict(seed=42, error=0.25, sha256="a" * 64, input="D:/developer/photos/target.png",
                      compiler="MSVC|19|C:/Program Files/Tools/cl.exe||cmake 4.2")
        result = public_report(source)
        self.assertEqual((result["seed"], result["error"], result["sha256"]), (42, 0.25, "a" * 64))
        self.assertNotIn("D:/", str(result))
        self.assertNotIn("C:/", str(result))
        self.assertTrue(result["paths_redacted"])

    def test_private_snapshot_closes_handles_and_preserves_relative_payloads(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "source"
            source.mkdir()
            (source / "image.png").write_bytes(b"fixture")
            (source / "data.tiny").write_bytes(bytes(256))
            (source / "data.hist").write_bytes(bytes(1024))
            connection = sqlite3.connect(source / "mosaicraft.db")
            connection.executescript("CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT);"
                "INSERT INTO meta VALUES('path_format','db-relative-v1');"
                "CREATE TABLE images(id INTEGER PRIMARY KEY,file_path TEXT UNIQUE,tiny_path TEXT,hist_path TEXT);"
                "INSERT INTO images VALUES(1,'image.png','data.tiny','data.hist');")
            connection.close()
            target, count = private_library(source / "mosaicraft.db", root / "study", images=True)
            self.assertEqual(count, 1)
            with readonly_database(target) as connection:
                paths = connection.execute("SELECT file_path,tiny_path,hist_path FROM images").fetchone()
                self.assertTrue(all(resource(target, path, "db-relative-v1").exists() for path in paths))
            target.parent.rename(root / "moved")


if __name__ == "__main__":
    unittest.main()
