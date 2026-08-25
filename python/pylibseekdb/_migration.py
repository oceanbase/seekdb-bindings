"""Shared implementation for the seekdb-dump and seekdb-restore commands."""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import datetime as dt
import decimal
import importlib.metadata
import itertools
import json
import os
import pathlib
import re
import sys
from collections.abc import Iterable, Iterator, Sequence
from typing import Any, TextIO

import pymysql
from pymysql.cursors import Cursor, SSCursor

import pylibseekdb


FORMAT_VERSION = 1
MAX_INSERT_ROWS = 1000
MAX_INSERT_BYTES = 1024 * 1024
SYSTEM_DATABASES = frozenset(
    {
        "information_schema",
        "mysql",
        "oceanbase",
        "performance_schema",
        "sys",
    }
)


class MigrationError(RuntimeError):
    """A runtime, SQL, file, or format error (exit status 1)."""


class PreflightError(MigrationError):
    """A safety or unsupported-object failure (exit status 2)."""


class SqlParseError(MigrationError):
    def __init__(self, line: int, message: str):
        super().__init__(f"line {line}: {message}")
        self.line = line


@dataclasses.dataclass(frozen=True, order=True)
class DbObject:
    database: str
    name: str
    kind: str


@dataclasses.dataclass(frozen=True, order=True)
class UnsupportedObject:
    kind: str
    database: str
    name: str
    reason: str


@dataclasses.dataclass
class DumpInventory:
    databases: list[str]
    tables: list[DbObject]
    views: list[DbObject]
    view_dependencies: dict[tuple[str, str], set[tuple[str, str]]]
    unsupported: list[UnsupportedObject]


@dataclasses.dataclass
class DumpMetadata:
    incomplete: bool = False
    skipped: list[UnsupportedObject] = dataclasses.field(default_factory=list)
    expected_rows: dict[tuple[str, str], int] = dataclasses.field(default_factory=dict)


def quote_identifier(value: str) -> str:
    return "`" + value.replace("`", "``") + "`"


def qualified_name(database: str, name: str) -> str:
    return f"{quote_identifier(database)}.{quote_identifier(name)}"


def source_version() -> str:
    try:
        return importlib.metadata.version("pylibseekdb")
    except importlib.metadata.PackageNotFoundError:
        return str(getattr(pylibseekdb, "__version__", "unknown"))


def is_system_database(name: str) -> bool:
    return name.casefold() in SYSTEM_DATABASES


@contextlib.contextmanager
def open_instance_connection(
    db_dir: str,
    *,
    cursorclass: type[Cursor] = Cursor,
    autocommit: bool,
) -> Iterator[tuple[Any, pymysql.Connection]]:
    instance = pylibseekdb.open(db_dir)
    connection: pymysql.Connection | None = None
    try:
        options = dict(instance.connection_options())
        connection = pymysql.connect(
            charset="utf8mb4",
            cursorclass=cursorclass,
            autocommit=autocommit,
            **options,
        )
        yield instance, connection
    finally:
        if connection is not None:
            connection.close()
        instance.close()
        pylibseekdb.close()


def fetch_scalar_rows(cursor: Cursor, sql: str, args: Sequence[Any] | None = None) -> list[str]:
    cursor.execute(sql, args)
    return [str(row[0]) for row in cursor.fetchall()]


def _placeholders(values: Sequence[Any]) -> str:
    return ", ".join(["%s"] * len(values))


def _fetch_unsupported_schema_objects(
    cursor: Cursor, databases: list[str]
) -> list[UnsupportedObject]:
    if not databases:
        return []
    placeholders = _placeholders(databases)
    unsupported: list[UnsupportedObject] = []

    inspections = (
        (
            "TRIGGER",
            "SELECT TRIGGER_SCHEMA, TRIGGER_NAME "
            f"FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA IN ({placeholders})",
            "triggers are not exported",
            False,
        ),
        (
            None,
            "SELECT ROUTINE_SCHEMA, ROUTINE_NAME, ROUTINE_TYPE "
            f"FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA IN ({placeholders})",
            "stored routines are not exported",
            False,
        ),
        (
            "EVENT",
            "SELECT EVENT_SCHEMA, EVENT_NAME "
            f"FROM information_schema.EVENTS WHERE EVENT_SCHEMA IN ({placeholders})",
            "events are not exported",
            True,
        ),
    )
    for fixed_kind, sql, reason, absent_means_unsupported_feature in inspections:
        try:
            cursor.execute(sql, databases)
            rows = cursor.fetchall()
        except Exception as exc:
            error_code = exc.args[0] if getattr(exc, "args", ()) else None
            if absent_means_unsupported_feature and error_code in {1109, 1146}:
                continue
            raise PreflightError(
                f"cannot inspect unsupported objects through information_schema: {exc}"
            ) from exc
        for row in rows:
            kind = fixed_kind or str(row[2]).upper()
            unsupported.append(
                UnsupportedObject(kind, str(row[0]), str(row[1]), reason)
            )
    return unsupported


def inspect_dump(cursor: Cursor, requested_databases: list[str]) -> DumpInventory:
    available = fetch_scalar_rows(cursor, "SHOW DATABASES")
    user_databases = sorted(name for name in available if not is_system_database(name))
    if requested_databases:
        available_by_fold = {name.casefold(): name for name in user_databases}
        missing = [name for name in requested_databases if name.casefold() not in available_by_fold]
        if missing:
            raise PreflightError("database not found: " + ", ".join(missing))
        databases = []
        seen: set[str] = set()
        for requested in requested_databases:
            actual = available_by_fold[requested.casefold()]
            if actual.casefold() not in seen:
                databases.append(actual)
                seen.add(actual.casefold())
    else:
        databases = user_databases

    tables: list[DbObject] = []
    views: list[DbObject] = []
    unsupported: list[UnsupportedObject] = []
    for database in databases:
        cursor.execute(f"SHOW FULL TABLES FROM {quote_identifier(database)}")
        for row in cursor.fetchall():
            name, raw_kind = str(row[0]), str(row[1]).upper()
            if raw_kind == "BASE TABLE":
                tables.append(DbObject(database, name, raw_kind))
            elif raw_kind == "VIEW":
                views.append(DbObject(database, name, raw_kind))
            else:
                unsupported.append(
                    UnsupportedObject(
                        raw_kind,
                        database,
                        name,
                        f"table type {raw_kind!r} is not exported",
                    )
                )

    unsupported.extend(_fetch_unsupported_schema_objects(cursor, databases))

    view_keys = {(view.database, view.name) for view in views}
    view_dependencies = {key: set() for key in view_keys}
    if views:
        placeholders = _placeholders(databases)
        try:
            cursor.execute(
                "SELECT VIEW_SCHEMA, VIEW_NAME, TABLE_SCHEMA, TABLE_NAME "
                "FROM information_schema.VIEW_TABLE_USAGE "
                f"WHERE VIEW_SCHEMA IN ({placeholders})",
                databases,
            )
            usage_rows = cursor.fetchall()
        except Exception as exc:
            raise PreflightError(f"cannot inspect view dependencies: {exc}") from exc
        selected_databases = {database.casefold() for database in databases}
        for view_schema, view_name, table_schema, table_name in usage_rows:
            view_key = (str(view_schema), str(view_name))
            if view_key not in view_dependencies:
                continue
            dependency = (str(table_schema), str(table_name))
            if dependency in view_keys:
                view_dependencies[view_key].add(dependency)
            elif (
                not is_system_database(dependency[0])
                and dependency[0].casefold() not in selected_databases
            ):
                unsupported.append(
                    UnsupportedObject(
                        "VIEW",
                        view_key[0],
                        view_key[1],
                        f"depends on database {dependency[0]!r}, which is not selected",
                    )
                )

    return DumpInventory(
        databases=databases,
        tables=sorted(tables),
        views=sorted(views),
        view_dependencies=view_dependencies,
        unsupported=sorted(set(unsupported)),
    )


def order_views(inventory: DumpInventory) -> list[DbObject]:
    by_key = {(view.database, view.name): view for view in inventory.views}
    dependencies = {key: set(value) for key, value in inventory.view_dependencies.items()}
    ordered: list[DbObject] = []
    while dependencies:
        ready = sorted(key for key, value in dependencies.items() if not value)
        if not ready:
            cycle = ", ".join(f"{db}.{name}" for db, name in sorted(dependencies))
            raise PreflightError(f"cyclic or unresolved view dependencies: {cycle}")
        for key in ready:
            ordered.append(by_key[key])
            dependencies.pop(key)
        for value in dependencies.values():
            value.difference_update(ready)
    return ordered


def _json_comment(prefix: str, payload: dict[str, Any]) -> str:
    return f"-- seekdb-dump-{prefix}: {json.dumps(payload, ensure_ascii=False, sort_keys=True)}\n"


def write_dump_header(output: TextIO, unsupported: list[UnsupportedObject]) -> None:
    output.write(f"-- seekdb-dump-format: {FORMAT_VERSION}\n")
    output.write(f"-- seekdb-dump-source-version: {source_version()}\n")
    output.write(f"-- seekdb-dump-incomplete: {'true' if unsupported else 'false'}\n")
    for item in unsupported:
        output.write(
            _json_comment(
                "skipped",
                dataclasses.asdict(item),
            )
        )
    output.write("-- Generated by seekdb-dump. Import into SeekDB or through a mysql client.\n\n")
    output.write("SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT;\n")
    output.write("SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS;\n")
    output.write("SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION;\n")
    output.write("SET NAMES utf8mb4;\n")
    output.write("SET @OLD_SQL_MODE=@@SQL_MODE;\n")
    output.write("SET SQL_MODE='NO_AUTO_VALUE_ON_ZERO';\n")
    output.write("SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS;\n")
    output.write("SET FOREIGN_KEY_CHECKS=0;\n\n")


def write_dump_footer(output: TextIO) -> None:
    output.write("\nSET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS;\n")
    output.write("SET SQL_MODE=@OLD_SQL_MODE;\n")
    output.write("SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT;\n")
    output.write("SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS;\n")
    output.write("SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION;\n")


def _show_create(cursor: Cursor, kind: str, database: str, name: str | None = None) -> str:
    if kind == "DATABASE":
        cursor.execute(f"SHOW CREATE DATABASE {quote_identifier(database)}")
    else:
        assert name is not None
        cursor.execute(f"SHOW CREATE {kind} {qualified_name(database, name)}")
    row = cursor.fetchone()
    if not row or len(row) < 2:
        raise MigrationError(f"SHOW CREATE {kind} returned no definition")
    return str(row[1]).rstrip().rstrip(";")


def _create_database_if_missing(ddl: str) -> str:
    return re.sub(
        r"^CREATE\s+DATABASE(?!\s+(?:IF\s+NOT\s+EXISTS|/\*!))",
        "CREATE DATABASE IF NOT EXISTS",
        ddl,
        count=1,
        flags=re.IGNORECASE,
    )


_DEFINER_RE = re.compile(
    r"\s+DEFINER\s*=\s*(?:`(?:``|[^`])*`@`(?:``|[^`])*`|'(?:''|[^'])*'@'(?:''|[^'])*'|[^\s]+)",
    re.IGNORECASE,
)


def strip_view_definer(ddl: str) -> str:
    return _DEFINER_RE.sub("", ddl, count=1)


def serialize_value(connection: pymysql.Connection, value: Any) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, (bytes, bytearray, memoryview)):
        return "0x" + bytes(value).hex().upper()
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(
        value,
        (int, float, decimal.Decimal, str, dt.date, dt.datetime, dt.time, dt.timedelta),
    ):
        escaped = connection.escape(value)
        if isinstance(escaped, bytes):
            return escaped.decode("ascii")
        return str(escaped)
    raise MigrationError(f"cannot serialize value of type {type(value).__name__}")


def _insert_prefix(table: DbObject, columns: list[str]) -> str:
    column_sql = ", ".join(quote_identifier(column) for column in columns)
    return f"INSERT INTO {qualified_name(table.database, table.name)} ({column_sql}) VALUES\n"


def _write_insert_batch(output: TextIO, prefix: str, rows: list[str]) -> None:
    if rows:
        output.write(prefix)
        output.write(",\n".join(rows))
        output.write(";\n")


def _dump_table_data(
    connection: pymysql.Connection,
    output: TextIO,
    table: DbObject,
) -> int:
    with connection.cursor(Cursor) as cursor:
        cursor.execute(
            "SELECT COLUMN_NAME, EXTRA FROM information_schema.COLUMNS "
            "WHERE TABLE_SCHEMA=%s AND TABLE_NAME=%s ORDER BY ORDINAL_POSITION",
            (table.database, table.name),
        )
        columns = [
            str(row[0])
            for row in cursor.fetchall()
            if "generated" not in str(row[1]).lower()
        ]
    if not columns:
        return 0

    select_columns = ", ".join(quote_identifier(column) for column in columns)
    sql = f"SELECT {select_columns} FROM {qualified_name(table.database, table.name)}"
    prefix = _insert_prefix(table, columns)
    prefix_bytes = len(prefix.encode("utf-8"))
    count = 0
    batch: list[str] = []
    batch_bytes = prefix_bytes
    with connection.cursor(SSCursor) as cursor:
        cursor.execute(sql)
        for row in cursor:
            row_sql = "(" + ", ".join(serialize_value(connection, value) for value in row) + ")"
            row_bytes = len(row_sql.encode("utf-8")) + (2 if batch else 0)
            if batch and (
                len(batch) >= MAX_INSERT_ROWS
                or batch_bytes + row_bytes > MAX_INSERT_BYTES
            ):
                _write_insert_batch(output, prefix, batch)
                batch = []
                batch_bytes = prefix_bytes
            batch.append(row_sql)
            batch_bytes += row_bytes
            count += 1
    _write_insert_batch(output, prefix, batch)
    return count


def dump_database(connection: pymysql.Connection, output: TextIO, inventory: DumpInventory) -> None:
    # SeekDB's default SHOW CREATE output includes physical single-node
    # properties such as REPLICA_NUM and block layout knobs. They are not
    # logical schema and have changed between SeekDB releases. The session
    # compatibility mode keeps portable SQL while retaining logical indexes,
    # generated columns, VECTOR definitions, charset, and collation.
    with connection.cursor(Cursor) as cursor:
        cursor.execute("SET SESSION _show_ddl_in_compat_mode=1")

    write_dump_header(output, inventory.unsupported)
    with connection.cursor(Cursor) as cursor:
        for database in inventory.databases:
            ddl = _create_database_if_missing(_show_create(cursor, "DATABASE", database))
            output.write(f"\n-- Database: {quote_identifier(database)}\n{ddl};\n")

        for table in inventory.tables:
            ddl = _show_create(cursor, "TABLE", table.database, table.name)
            output.write(
                f"\n-- Table: {qualified_name(table.database, table.name)}\n"
                f"USE {quote_identifier(table.database)};\n{ddl};\n"
            )

    for table in inventory.tables:
        output.write(f"\n-- Data: {qualified_name(table.database, table.name)}\n")
        row_count = _dump_table_data(connection, output, table)
        output.write(
            _json_comment(
                "table",
                {"database": table.database, "rows": row_count, "table": table.name},
            )
        )

    with connection.cursor(Cursor) as cursor:
        for view in order_views(inventory):
            ddl = strip_view_definer(_show_create(cursor, "VIEW", view.database, view.name))
            output.write(
                f"\n-- View: {qualified_name(view.database, view.name)}\n"
                f"USE {quote_identifier(view.database)};\n{ddl};\n"
            )
    write_dump_footer(output)


def report_unsupported(items: Iterable[UnsupportedObject], *, error: bool) -> None:
    items = list(items)
    if not items:
        return
    level = "ERROR" if error else "WARNING"
    print(f"{level}: unsupported objects found:", file=sys.stderr)
    for item in items:
        print(
            f"  {item.kind} {item.database}.{item.name}: {item.reason}",
            file=sys.stderr,
        )


@contextlib.contextmanager
def dump_output(path: str | None) -> Iterator[TextIO]:
    if path is None:
        yield sys.stdout
        return
    destination = pathlib.Path(path)
    temporary = destination.with_name(f".{destination.name}.tmp-{os.getpid()}")
    try:
        with temporary.open("w", encoding="utf-8", newline="\n") as stream:
            yield stream
        os.replace(temporary, destination)
    finally:
        with contextlib.suppress(FileNotFoundError):
            temporary.unlink()


def run_dump(args: argparse.Namespace) -> int:
    print(
        "NOTICE: system databases, users, and grants are not included in the dump.",
        file=sys.stderr,
    )
    with open_instance_connection(args.db_dir, cursorclass=Cursor, autocommit=False) as (
        _instance,
        connection,
    ):
        with connection.cursor(Cursor) as cursor:
            inventory = inspect_dump(cursor, args.database)
        # Resolve dependency errors before any output is created.
        order_views(inventory)
        if inventory.unsupported and not args.ignore_unsupported:
            report_unsupported(inventory.unsupported, error=True)
            print("No dump was written.", file=sys.stderr)
            print(
                "Use --ignore-unsupported to export supported objects only.",
                file=sys.stderr,
            )
            raise PreflightError("unsupported objects prevent a complete dump")
        if inventory.unsupported:
            report_unsupported(inventory.unsupported, error=False)

        with connection.cursor(Cursor) as cursor:
            cursor.execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ")
            cursor.execute("START TRANSACTION WITH CONSISTENT SNAPSHOT")
        try:
            with dump_output(args.output) as output:
                dump_database(connection, output, inventory)
        finally:
            connection.rollback()
    return 0


_HEADER_RE = re.compile(r"^--\s*seekdb-dump-([a-z-]+):\s*(.*?)\s*$")


def parse_metadata_line(line: str, metadata: DumpMetadata) -> None:
    match = _HEADER_RE.match(line)
    if not match:
        return
    key, raw_value = match.groups()
    try:
        if key == "incomplete":
            if raw_value.casefold() not in {"true", "false"}:
                raise ValueError("expected true or false")
            metadata.incomplete = raw_value.casefold() == "true"
        elif key == "skipped":
            value = json.loads(raw_value)
            metadata.skipped.append(UnsupportedObject(**value))
        elif key == "table":
            value = json.loads(raw_value)
            metadata.expected_rows[(str(value["database"]), str(value["table"]))] = int(
                value["rows"]
            )
    except (KeyError, TypeError, ValueError) as exc:
        raise MigrationError(f"invalid seekdb dump metadata for {key}: {exc}") from exc


def read_metadata_prefix(stream: TextIO, metadata: DumpMetadata) -> list[str]:
    prefix: list[str] = []
    for line in stream:
        prefix.append(line)
        parse_metadata_line(line.rstrip("\r\n"), metadata)
        stripped = line.lstrip()
        if stripped and not stripped.startswith("--"):
            break
    return prefix


def iter_sql_statements(
    lines: Iterable[str], metadata: DumpMetadata | None = None
) -> Iterator[tuple[int, str]]:
    buffer: list[str] = []
    state = "normal"
    escaped = False
    statement_line = 1
    line_number = 0

    for line_number, line in enumerate(lines, 1):
        if metadata is not None:
            parse_metadata_line(line.rstrip("\r\n"), metadata)
        if not buffer and not line.strip():
            statement_line = line_number + 1
        index = 0
        while index < len(line):
            char = line[index]
            nxt = line[index + 1] if index + 1 < len(line) else ""
            third = line[index + 2] if index + 2 < len(line) else ""
            buffer.append(char)

            if state == "line-comment":
                if char == "\n":
                    state = "normal"
            elif state == "block-comment":
                if char == "*" and nxt == "/":
                    buffer.append(nxt)
                    index += 1
                    state = "normal"
            elif state in {"single", "double", "backtick"}:
                quote = {"single": "'", "double": '"', "backtick": "`"}[state]
                if escaped:
                    escaped = False
                elif char == "\\" and state != "backtick":
                    escaped = True
                elif char == quote:
                    if nxt == quote:
                        buffer.append(nxt)
                        index += 1
                    else:
                        state = "normal"
            else:
                if char == "#":
                    state = "line-comment"
                elif char == "-" and nxt == "-" and (not third or third.isspace()):
                    buffer.append(nxt)
                    index += 1
                    state = "line-comment"
                elif char == "/" and nxt == "*":
                    buffer.append(nxt)
                    index += 1
                    state = "block-comment"
                elif char == "'":
                    state = "single"
                elif char == '"':
                    state = "double"
                elif char == "`":
                    state = "backtick"
                elif char == ";":
                    statement = "".join(buffer)
                    if _has_executable_sql(statement):
                        yield statement_line, statement
                    buffer = []
                    statement_line = line_number
            index += 1

    if state in {"single", "double", "backtick", "block-comment"}:
        raise SqlParseError(statement_line, f"unterminated {state.replace('-', ' ')}")
    statement = "".join(buffer)
    if _has_executable_sql(statement):
        yield statement_line, statement


_LEADING_COMMENT_RE = re.compile(
    r"\A(?:\s+|--[^\n]*(?:\n|\Z)|\#[^\n]*(?:\n|\Z)|/\*(?!\!)[\s\S]*?\*/)*",
    re.MULTILINE,
)


def _has_executable_sql(statement: str) -> bool:
    remainder = _LEADING_COMMENT_RE.sub("", statement, count=1)
    return bool(remainder.strip(" ;\r\n\t"))


def statement_summary(statement: str, limit: int = 160) -> str:
    summary = " ".join(statement.split())
    if len(summary) > limit:
        return summary[: limit - 3] + "..."
    return summary


def inspect_empty_target(cursor: Cursor) -> None:
    databases = fetch_scalar_rows(cursor, "SHOW DATABASES")
    found: list[str] = []
    for database in databases:
        if is_system_database(database):
            continue
        cursor.execute(f"SHOW FULL TABLES FROM {quote_identifier(database)}")
        for row in cursor.fetchall():
            found.append(f"{database}.{row[0]} ({row[1]})")
    if found:
        details = ", ".join(found[:10])
        if len(found) > 10:
            details += f", and {len(found) - 10} more"
        raise PreflightError(f"target instance contains user objects: {details}")


def validate_row_counts(connection: pymysql.Connection, metadata: DumpMetadata) -> None:
    mismatches: list[str] = []
    with connection.cursor() as cursor:
        for (database, table), expected in sorted(metadata.expected_rows.items()):
            cursor.execute(f"SELECT COUNT(*) FROM {qualified_name(database, table)}")
            row = cursor.fetchone()
            actual = int(row[0]) if row else -1
            if actual != expected:
                mismatches.append(f"{database}.{table}: expected {expected}, got {actual}")
    if mismatches:
        raise MigrationError("row-count validation failed: " + "; ".join(mismatches))


def run_restore(args: argparse.Namespace) -> int:
    input_stream: TextIO
    should_close = False
    if args.sql_file:
        input_stream = pathlib.Path(args.sql_file).open("r", encoding="utf-8", newline="")
        should_close = True
    else:
        input_stream = sys.stdin

    metadata = DumpMetadata()
    try:
        prefix = read_metadata_prefix(input_stream, metadata)
        if metadata.incomplete:
            print(
                "WARNING: dump is marked incomplete; skipped objects will not be restored.",
                file=sys.stderr,
            )
            report_unsupported(metadata.skipped, error=False)

        with open_instance_connection(args.db_dir, cursorclass=Cursor, autocommit=True) as (
            _instance,
            connection,
        ):
            with connection.cursor() as cursor:
                inspect_empty_target(cursor)
                for line, statement in iter_sql_statements(
                    itertools.chain(prefix, input_stream), metadata
                ):
                    executable = _LEADING_COMMENT_RE.sub("", statement, count=1).lstrip()
                    if executable.upper().startswith("DELIMITER"):
                        raise SqlParseError(line, "DELIMITER and stored programs are not supported")
                    try:
                        cursor.execute(statement)
                    except Exception as exc:
                        raise MigrationError(
                            f"line {line}: {statement_summary(statement)}: {exc}"
                        ) from exc
            validate_row_counts(connection, metadata)
    finally:
        if should_close:
            input_stream.close()
    return 0


def dump_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="seekdb-dump",
        description="Dump an embedded SeekDB instance as mysql-compatible SQL.",
    )
    parser.add_argument("db_dir", metavar="DB_DIR")
    parser.add_argument(
        "--database",
        action="append",
        default=[],
        metavar="NAME",
        help="dump only this database; repeat to select more than one",
    )
    parser.add_argument(
        "-o",
        "--output",
        metavar="FILE",
        help="write SQL to FILE instead of stdout",
    )
    parser.add_argument(
        "--ignore-unsupported",
        action="store_true",
        help="skip unsupported objects and mark the dump incomplete",
    )
    return parser


def restore_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="seekdb-restore",
        description="Restore mysql-compatible SQL into an empty embedded SeekDB instance.",
    )
    parser.add_argument("db_dir", metavar="DB_DIR")
    parser.add_argument("sql_file", metavar="SQL_FILE", nargs="?")
    return parser


def cli_main(
    parser: argparse.ArgumentParser,
    runner: Any,
    argv: Sequence[str] | None = None,
) -> int:
    args = parser.parse_args(argv)
    try:
        return int(runner(args))
    except PreflightError as exc:
        if str(exc) and "unsupported objects prevent" not in str(exc):
            print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    except BrokenPipeError:
        with contextlib.suppress(OSError):
            sys.stdout.close()
        return 1
    except (MigrationError, OSError, pymysql.MySQLError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


def dump_main(argv: Sequence[str] | None = None) -> int:
    return cli_main(dump_parser(), run_dump, argv)


def restore_main(argv: Sequence[str] | None = None) -> int:
    return cli_main(restore_parser(), run_restore, argv)
