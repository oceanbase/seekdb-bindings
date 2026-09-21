"""End-to-end dump/restore smoke test against the wheel's embedded runtime."""

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

import pylibseekdb

from pylibseekdb._migration import dump_main, restore_main


def execute(instance, database, statements):
    connection = instance.connect(database, autocommit=True)
    cursor = connection.cursor()
    try:
        for statement in statements:
            cursor.execute(statement)
    finally:
        cursor.close()
        connection.close()


def create_source(db_dir):
    instance = pylibseekdb.open(str(db_dir))
    try:
        execute(instance, "test", ["CREATE DATABASE migration_case"])
        execute(
            instance,
            "migration_case",
            [
                """CREATE TABLE documents (
                     id BIGINT AUTO_INCREMENT PRIMARY KEY,
                     payload BLOB,
                     empty_payload BLOB,
                     empty_binary VARBINARY(8),
                     note VARCHAR(255),
                     metadata JSON,
                     embedding VECTOR(3),
                     event_time TIMESTAMP,
                     base_value BIGINT,
                     hidden_note VARCHAR(16) INVISIBLE,
                     doubled BIGINT GENERATED ALWAYS AS (base_value * 2) STORED,
                     VECTOR INDEX idx_embedding (embedding)
                       WITH (DISTANCE=l2, TYPE=hnsw, LIB=vsag)
                   )""",
                "SET SESSION time_zone='+08:00'",
                """INSERT INTO documents(
                     payload, empty_payload, empty_binary, note, metadata,
                     embedding, event_time, base_value, hidden_note)
                   VALUES (X'000A27FF', X'', X'', 'line 1\\nline 2',
                           '{\"kind\":\"test\"}', '[0.1,0.2,0.3]',
                           '2020-01-01 08:00:00', 5, 'secret')""",
                "CREATE VIEW document_names AS SELECT id, note FROM documents",
            ],
        )
    finally:
        instance.close()
        pylibseekdb.close()


def create_trigger(db_dir):
    instance = pylibseekdb.open(str(db_dir))
    try:
        statement = (
            "CREATE TRIGGER documents_before_insert BEFORE INSERT ON documents "
            "FOR EACH ROW SET NEW.note = NEW.note"
        )
        execute(instance, "migration_case", [statement])
    finally:
        instance.close()
        pylibseekdb.close()


def create_cross_database_views(db_dir):
    instance = pylibseekdb.open(str(db_dir))
    try:
        execute(
            instance,
            "test",
            ["CREATE DATABASE view_a", "CREATE DATABASE view_b"],
        )
        execute(instance, "view_b", ["CREATE TABLE source_table (id INT)"])
        execute(
            instance,
            "view_a",
            [
                "CREATE VIEW direct_view AS SELECT id FROM view_b.source_table",
                "CREATE VIEW indirect_view AS SELECT id FROM direct_view",
                "CREATE VIEW safe_view AS SELECT 1 AS id",
            ],
        )
    finally:
        instance.close()
        pylibseekdb.close()


def verify_target(db_dir):
    instance = pylibseekdb.open(str(db_dir))
    connection = instance.connect("migration_case", autocommit=True)
    cursor = connection.cursor()
    try:
        cursor.execute(
            "SELECT id, HEX(payload), HEX(empty_payload), HEX(empty_binary), note, "
            "JSON_EXTRACT(metadata, '$.kind'), UNIX_TIMESTAMP(event_time), "
            "hidden_note, doubled "
            "FROM documents"
        )
        row = cursor.fetchone()
        assert row[0] == 1
        assert row[1] == "000A27FF"
        assert row[2] == ""
        assert row[3] == ""
        assert row[4] == "line 1\nline 2"
        assert str(row[5]).strip('"') == "test"
        assert row[6] == 1577836800
        assert row[7] == "secret"
        assert row[8] == 10
        cursor.execute("SELECT COUNT(*) FROM document_names")
        assert cursor.fetchone()[0] == 1
        cursor.execute("SHOW INDEX FROM documents")
        index_names = {row[2] for row in cursor.fetchall()}
        assert "idx_embedding" in index_names
    finally:
        cursor.close()
        connection.close()
        instance.close()
        pylibseekdb.close()


def restore_with_mysql(db_dir, sql_file):
    mysql = shutil.which("mysql") or shutil.which("mariadb")
    if mysql is None:
        print("mysql client not found; skipping external-client restore")
        return False
    instance = pylibseekdb.open(str(db_dir))
    try:
        options = instance.connection_options()
        command = [mysql, "--user=root", "--default-character-set=utf8mb4"]
        if "unix_socket" in options:
            command.extend(["--protocol=socket", f"--socket={options['unix_socket']}"])
        else:
            command.extend(["--protocol=tcp", "--host=127.0.0.1", f"--port={options['port']}"])
        with sql_file.open("rb") as input_stream:
            subprocess.run(command, stdin=input_stream, check=True)
    finally:
        instance.close()
        pylibseekdb.close()
    return True


def main():
    data_root = os.environ.get("SEEKDB_TEST_DATA_ROOT", str(pathlib.Path.cwd()))
    with tempfile.TemporaryDirectory(prefix="pylibseekdb-migration-", dir=data_root) as root:
        root_path = pathlib.Path(root)
        source = root_path / "source.db"
        unsupported_source = root_path / "unsupported-source.db"
        cross_view_source = root_path / "cross-view-source.db"
        target = root_path / "target.db"
        mysql_target = root_path / "mysql-target.db"
        incomplete_target = root_path / "incomplete-target.db"
        cross_view_target = root_path / "cross-view-target.db"
        truncated_target = root_path / "truncated-target.db"
        sql_file = root_path / "backup.sql"
        rejected_sql_file = root_path / "rejected.sql"
        incomplete_sql_file = root_path / "incomplete.sql"
        cross_view_sql_file = root_path / "cross-view.sql"
        truncated_sql_file = root_path / "truncated.sql"

        blocked_parent = root_path / "not-a-directory"
        blocked_parent.write_text("not a directory", encoding="utf-8")
        failed_cli = subprocess.run(
            [
                sys.executable,
                "-m",
                "pylibseekdb.dump",
                str(blocked_parent / "source.db"),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        assert failed_cli.returncode == 1
        assert "ERROR:" in failed_cli.stderr
        assert "Traceback" not in failed_cli.stderr

        create_source(unsupported_source)
        create_trigger(unsupported_source)
        assert dump_main([str(unsupported_source), "-o", str(rejected_sql_file)]) == 2
        assert not rejected_sql_file.exists()
        assert (
            dump_main(
                [
                    str(unsupported_source),
                    "--ignore-unsupported",
                    "-o",
                    str(incomplete_sql_file),
                ]
            )
            == 0
        )
        assert "seekdb-dump-incomplete: true" in incomplete_sql_file.read_text(
            encoding="utf-8"
        )
        assert restore_main([str(incomplete_target), str(incomplete_sql_file)]) == 0
        verify_target(incomplete_target)

        create_cross_database_views(cross_view_source)
        assert (
            dump_main(
                [
                    str(cross_view_source),
                    "--database",
                    "view_a",
                    "--ignore-unsupported",
                    "-o",
                    str(cross_view_sql_file),
                ]
            )
            == 0
        )
        cross_view_sql = cross_view_sql_file.read_text(encoding="utf-8")
        assert cross_view_sql.count('"name": "direct_view"') == 1
        assert cross_view_sql.count('"name": "indirect_view"') == 1
        assert cross_view_sql.count('"name": "safe_view"') == 1
        assert restore_main([str(cross_view_target), str(cross_view_sql_file)]) == 0

        create_source(source)
        assert dump_main([str(source), "-o", str(sql_file)]) == 0
        dump_sql = sql_file.read_text(encoding="utf-8")
        assert "0x000A27FF" in dump_sql
        assert "X''" in dump_sql
        assert "seekdb-dump-incomplete: false" in dump_sql
        assert dump_sql.rstrip().endswith("-- seekdb-dump-complete: true")
        assert "GENERATED ALWAYS" in dump_sql.upper()

        insert_offset = dump_sql.index("\nINSERT INTO ")
        truncated_sql_file.write_text(dump_sql[:insert_offset], encoding="utf-8")
        assert restore_main([str(truncated_target), str(truncated_sql_file)]) == 1

        assert restore_main([str(target), str(sql_file)]) == 0
        verify_target(target)

        # A restored instance is not an empty target and must be rejected
        # before any SQL is executed.
        assert restore_main([str(target), str(sql_file)]) == 2

        if restore_with_mysql(mysql_target, sql_file):
            verify_target(mysql_target)

    print("migration integration test passed")


if __name__ == "__main__":
    main()
