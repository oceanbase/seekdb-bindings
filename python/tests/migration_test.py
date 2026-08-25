"""Focused tests for seekdb dump/restore formatting and parsing."""

from __future__ import annotations

import datetime as dt
import decimal
import io
import json

import pymysql.converters

from pylibseekdb._migration import (
    DbObject,
    DumpInventory,
    DumpMetadata,
    SqlParseError,
    UnsupportedObject,
    _create_database_if_missing,
    iter_sql_statements,
    order_views,
    parse_metadata_line,
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
    parse_metadata_line("-- seekdb-dump-incomplete: true", metadata)
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
    assert metadata.incomplete
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


def main():
    test_identifiers_and_ddl_rewrites()
    test_serialize_values()
    test_sql_stream_parser()
    test_sql_stream_parser_reports_unterminated_input()
    test_metadata_comments()
    test_view_dependency_order()
    print("migration tests passed")


if __name__ == "__main__":
    main()
