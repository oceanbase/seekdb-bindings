"""Focused tests for seekdb dump/restore formatting and parsing."""

from __future__ import annotations

import contextlib
import datetime as dt
import decimal
import io
import json

import pymysql.converters
import pylibseekdb

from pylibseekdb._migration import (
    DbObject,
    DumpInventory,
    DumpMetadata,
    SqlParseError,
    UnsupportedObject,
    _create_database_if_missing,
    _object_comment,
    dump_main,
    iter_sql_statements,
    order_views,
    parse_metadata_line,
    prune_unsupported_views,
    quote_identifier,
    serialize_value,
    strip_view_definer,
)


class EscapingConnection:
    def escape(self, value):
        return pymysql.converters.escape_item(
            value,
            "utf8mb4",
            mapping=pymysql.converters.encoders,
        )


def test_identifiers_and_ddl_rewrites():
    assert quote_identifier("a`b") == "`a``b`"
    assert (
        _create_database_if_missing("CREATE DATABASE `app` DEFAULT CHARACTER SET utf8mb4")
        == "CREATE DATABASE IF NOT EXISTS `app` DEFAULT CHARACTER SET utf8mb4"
    )
    view = (
        "CREATE ALGORITHM=UNDEFINED DEFINER=`root`@`%` SQL SECURITY DEFINER "
        "VIEW `v` AS select 1 AS `x`"
    )
    assert "DEFINER=`root`@`%`" not in strip_view_definer(view)
    assert "SQL SECURITY DEFINER" in strip_view_definer(view)


def test_serialize_values():
    connection = EscapingConnection()
    assert serialize_value(connection, None) == "NULL"
    assert serialize_value(connection, b"\x00\n'\xff") == "0x000A27FF"
    assert serialize_value(connection, b"") == "X''"
    assert serialize_value(connection, memoryview(b"")) == "X''"
    assert serialize_value(connection, memoryview(b"\x01\x02")) == "0x0102"
    assert serialize_value(connection, True) == "1"
    assert serialize_value(connection, decimal.Decimal("12.340")) == "12.340"
    assert serialize_value(connection, dt.date(2026, 8, 25)) == "'2026-08-25'"
    assert serialize_value(connection, dt.timedelta(hours=25)) == "'25:00:00'"
    escaped = serialize_value(connection, "line 1\n'line 2'\\")
    assert escaped.startswith("'") and escaped.endswith("'")
    assert "\\n" in escaped and "\\'" in escaped


def test_sql_stream_parser():
    sql = """-- header
CREATE DATABASE `app`;
INSERT INTO `app`.`t` VALUES
  (1, 'semi;colon', 'quote\\'value'),
  (2, "double;quote", 0x000A);
/* ordinary comment; */
/*!40101 SET SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
CREATE VIEW `app`.`v` AS SELECT `semi;colon` AS `x` FROM `app`.`t`;
"""
    statements = list(iter_sql_statements(io.StringIO(sql)))
    assert len(statements) == 4
    assert statements[0][0] == 1
    assert "semi;colon" in statements[1][1]
    assert statements[-1][1].rstrip().endswith(";")


def test_sql_stream_parser_reports_unterminated_input():
    try:
        list(iter_sql_statements(["INSERT INTO t VALUES ('broken);\n"]))
    except SqlParseError as error:
        assert "unterminated single" in str(error)
    else:
        raise AssertionError("unterminated SQL was accepted")


def test_metadata_comments():
    metadata = DumpMetadata()
    parse_metadata_line("-- seekdb-dump-format: 1", metadata)
    parse_metadata_line("-- seekdb-dump-incomplete: true", metadata)
    parse_metadata_line("-- seekdb-dump-complete: true", metadata)
    skipped = {
        "kind": "TRIGGER",
        "database": "app",
        "name": "audit",
        "reason": "triggers are not exported",
    }
    parse_metadata_line(
        "-- seekdb-dump-skipped: " + json.dumps(skipped, sort_keys=True), metadata
    )
    parse_metadata_line(
        '-- seekdb-dump-table: {"database":"app","rows":3,"table":"t"}',
        metadata,
    )
    assert metadata.format_version == 1
    assert metadata.incomplete
    assert metadata.complete
    assert metadata.skipped == [UnsupportedObject(**skipped)]
    assert metadata.expected_rows == {("app", "t"): 3}


def test_view_dependency_order():
    first = DbObject("app", "first", "VIEW")
    second = DbObject("app", "second", "VIEW")
    third = DbObject("other", "third", "VIEW")
    inventory = DumpInventory(
        databases=["app", "other"],
        tables=[],
        views=[third, second, first],
        view_dependencies={
            ("app", "first"): set(),
            ("app", "second"): {("app", "first")},
            ("other", "third"): {("app", "second")},
        },
        unsupported=[],
    )
    assert order_views(inventory) == [first, second, third]


def test_prune_unsupported_views_transitively():
    direct = DbObject("app", "direct", "VIEW")
    indirect = DbObject("app", "indirect", "VIEW")
    safe = DbObject("app", "safe", "VIEW")
    views, dependencies, unsupported = prune_unsupported_views(
        [direct, indirect, safe],
        {
            ("app", "direct"): set(),
            ("app", "indirect"): {("app", "direct")},
            ("app", "safe"): set(),
        },
        {("app", "direct"): {"outside"}},
    )
    assert views == [safe]
    assert dependencies == {("app", "safe"): set()}
    assert {(item.database, item.name) for item in unsupported} == {
        ("app", "direct"),
        ("app", "indirect"),
    }


def test_object_comments_cannot_emit_sql():
    malicious_name = "evil\n; SELECT 424242; -- comment"
    comment = _object_comment("Table", "app", malicious_name)
    assert comment.count("\n") == 1
    assert "\\n" in comment
    sql = comment + f"CREATE TABLE {quote_identifier(malicious_name)} (`id` INT);\n"
    statements = list(iter_sql_statements(io.StringIO(sql)))
    assert len(statements) == 1
    assert "SELECT 424242" in statements[0][1]


def test_seekdb_error_is_reported_without_traceback():
    original_open = pylibseekdb.open

    def fail_open(_db_dir):
        raise pylibseekdb.SeekdbError("forced startup failure")

    pylibseekdb.open = fail_open
    stderr = io.StringIO()
    try:
        with contextlib.redirect_stderr(stderr):
            assert dump_main(["unused.db"]) == 1
    finally:
        pylibseekdb.open = original_open
    output = stderr.getvalue()
    assert "ERROR: forced startup failure" in output
    assert "Traceback" not in output


def main():
    test_identifiers_and_ddl_rewrites()
    test_serialize_values()
    test_sql_stream_parser()
    test_sql_stream_parser_reports_unterminated_input()
    test_metadata_comments()
    test_view_dependency_order()
    test_prune_unsupported_views_transitively()
    test_object_comments_cannot_emit_sql()
    test_seekdb_error_is_reported_without_traceback()
    print("migration tests passed")


if __name__ == "__main__":
    main()
