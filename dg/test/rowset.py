#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# rowset.py -- psql's output with each unordered result's rows sorted, so
# that two outputs compare as SQL compares results: a query without ORDER
# BY returns a multiset of rows, in whatever order its plan reads them. A
# table with a primary key is read in the key's order in digres
# (../../sm/CLUSTER.md), and in insertion order in PostgreSQL, so their
# outputs differ in order alone.
#
#   rowset.py FILE      print FILE so
#
# A result is psql's aligned table: a header, a line of dashes, its rows
# and "(N rows)". Its rows are sorted unless the statement before it says
# ORDER BY anywhere, a subquery's included, or the result is a plan, or a
# row runs over more than one line.
import re
import sys

RULE = re.compile(r'^-+(\+-+)*$')
COUNT = re.compile(r'^\((\d+) rows?\)$')
ORDERED = re.compile(r'\border\s+by\b', re.IGNORECASE)


def last_statement(lines):
    """The last statement psql echoed in lines, the text from the
    semicolon that ends the one before it."""
    pieces = [p for p in '\n'.join(lines).split(';') if p.strip()]
    return pieces[-1] if pieces else ''


def rowset(text):
    """text with each unordered result's rows sorted."""
    lines = text.split('\n')
    out = []
    stmt = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if (i + 1 < len(lines) and RULE.match(lines[i + 1].strip()) and
                line.strip()):
            # A result: its header, its rule, its rows to the count.
            end = i + 2
            while end < len(lines) and not COUNT.match(lines[end].strip()):
                end += 1
            if end == len(lines):
                out.append(line)
                i += 1
                continue
            rows = lines[i + 2:end]
            unordered = (not ORDERED.search(last_statement(stmt)) and
                         line.strip() != 'QUERY PLAN' and
                         not any(r.rstrip().endswith('+') for r in rows))
            out.extend(lines[i:i + 2])
            out.extend(sorted(rows) if unordered else rows)
            out.append(lines[end])
            stmt = []
            i = end + 1
            continue
        out.append(line)
        stmt.append(line)
        i += 1
    return '\n'.join(out)


if __name__ == '__main__':
    with open(sys.argv[1], encoding='latin-1') as f:
        sys.stdout.write(rowset(f.read()))
