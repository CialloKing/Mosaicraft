"""Process-level first-open race and WAL-backup check, with no image decoding."""
import argparse
import concurrent.futures
from pathlib import Path
import sqlite3
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    args = parser.parse_args()
    executable = args.cli.resolve()
    with tempfile.TemporaryDirectory(prefix="mosaicraft-portable-") as folder:
        root = Path(folder)
        library = root / "library A"
        library.mkdir()
        database = library / "mosaicraft.db"
        with sqlite3.connect(database) as connection:
            connection.executescript("""
                CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);
                INSERT INTO meta VALUES('feature_w','180'),('feature_h','320');
                CREATE TABLE images(id INTEGER PRIMARY KEY,file_path TEXT UNIQUE,file_hash TEXT,format TEXT,
                    src_width INTEGER,src_height INTEGER,aspect_ratio REAL,file_size INTEGER,
                    avg_l REAL,avg_a REAL,avg_b REAL,mean_brightness REAL,std_brightness REAL,contrast REAL,
                    saturation REAL,color_variance REAL,grid_lab BLOB,tiny_path TEXT,edge_density REAL,
                    lbp_path TEXT,use_count INTEGER,feature_version INTEGER);
            """)
            connection.executemany("INSERT INTO images VALUES(" + ",".join("?" for _ in range(22)) + ")",
                ((i, str(library / f"{i}.png"), str(i), "png", 180, 320, 180 / 320, 0,
                  128, 128, 128, 0.5, 0, 0, 0, 0, bytes(768), str(library / f"{i}.tiny"), 0,
                  str(library / f"{i}.hist"), 0, 6) for i in range(1, 2501)))
            connection.commit()
        connection.close()
        # Keep a connection alive so committed usage exists in WAL during backup.
        writer = sqlite3.connect(database)
        writer.execute("PRAGMA journal_mode=WAL")
        writer.execute("UPDATE images SET use_count=9 WHERE id=1")
        writer.commit()
        columns = [r[1] for r in writer.execute("PRAGMA table_info(images)") if r[1] not in
                   ("file_path", "tiny_path", "lbp_path")]
        query = "SELECT " + ",".join(columns) + " FROM images ORDER BY id"
        before = writer.execute(query).fetchall()

        def read():
            result = subprocess.run([str(executable), "db-stats", "-d", str(database)], cwd=root,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            assert result.returncode == 0, result.stdout.decode("utf8", errors="replace")

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as workers:
            list(workers.map(lambda _: read(), range(4)))
        assert writer.execute("SELECT value FROM meta WHERE key='path_format'").fetchone()[0] == "db-relative-v1"
        assert writer.execute(query).fetchall() == before
        assert all(not Path(r[0]).is_absolute() for r in writer.execute("SELECT file_path FROM images"))
        backups = list((library / ".mosaicraft-backups").glob("*.db"))
        assert len(backups) == 1
        backup = sqlite3.connect(backups[0])
        try:
            assert backup.execute("SELECT use_count FROM images WHERE id=1").fetchone()[0] == 9
            assert backup.execute("SELECT value FROM meta WHERE key='path_format'").fetchone() is None
        finally:
            backup.close()
        writer.close()
        moved = root / "library B"
        library.rename(moved)
        database = moved / "mosaicraft.db"
        read()
        assert len(list((moved / ".mosaicraft-backups").glob("*.db"))) == 1
        print("four readers, one upgrade, WAL backup and relocated database: passed")


if __name__ == "__main__":
    main()
