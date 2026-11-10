#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# report.py -- read what test/regress.sh left in WORK for each FILE, and
# write WORK/summary, WORK/causes, WORK/crashes and WORK/setup.
#
#   test/regress/report.py WORK FILE...
#
# A file's cause is the first error in its diff that PostgreSQL 18's
# expected output does not have, with names, numbers and quoted strings
# taken out, so that one missing feature is one cause however many
# The summary ends each line with the statement that raised that error.
# objects it is met on.
import os
import re
import sys
from collections import defaultdict

# The catalog each catalog table's or index's oid names, from the headers
# in src/include/catalog, so that "relation 1247 cannot be written" says
# pg_type once its oid is taken out.
CATALOG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           '..', '..', 'src', 'include', 'catalog')
CATALOGS = {}
for h in os.listdir(CATALOG_DIR):
    if h.endswith('.h'):
        with open(os.path.join(CATALOG_DIR, h)) as f:
            text = f.read()
        for name, oid in re.findall(r'^CATALOG\((\w+),\s*(\d+)', text, re.M):
            CATALOGS[oid] = name
        for oid, table in re.findall(
                r'^DECLARE_\w*INDEX\w*\(\w+,\s*(\d+),\s*\w+,\s*(\w+)',
                text, re.M):
            CATALOGS[oid] = table

# Lines a server log has when the server crashed.
CRASH = re.compile(r'TRAP:|terminated by signal|PANIC:|Sanitizer')

# Messages whose names are not quoted: the part to keep, and what the
# name is replaced with. Order matters: the first that matches is used.
UNQUOTED = [
    (re.compile(r'^type .+ is not supported yet$'),
     'type ? is not supported yet'),
    (re.compile(r'^(function|procedure|aggregate) \S+\(.*\) does not exist$'),
     r'\1 ?(...) does not exist'),
    (re.compile(r'^type [^" ]+ does not exist$'), 'type ? does not exist'),
    (re.compile(r'^operator does not exist: .*'),
     'operator does not exist: ?'),
    (re.compile(r'^(could not find array type for data type) .*'), r'\1 ?'),
    (re.compile(r'^(collations are not supported by type) .*'), r'\1 ?'),
    (re.compile(r'^(cannot cast type) .+ (to) .+'), r'\1 ? \2 ?'),
    (re.compile(r'^(column ".*" is of type) .+ (but expression is of type)'
                r' .+'), r'\1 ? \2 ?'),
    (re.compile(r'^(invalid input syntax for type) [^:]+(:.*)?$'), r'\1 ?'),
]


def normalize(msg):
    """Take the names, numbers and quoted strings out of an error."""
    msg = re.sub(r'^relation (\d+) (?=cannot be)',
                 lambda m: 'relation %s ' % CATALOGS.get(m.group(1), 'N'), msg)
    for pat, rep in UNQUOTED:
        if pat.match(msg):
            msg = pat.sub(rep, msg)
            break
    msg = re.sub(r'"[^"]*"', '"?"', msg)
    msg = re.sub(r"'[^']*'", "'?'", msg)
    msg = re.sub(r'\b\d+(\.\d+)?\b', 'N', msg)
    return msg


def read(path):
    """The lines of a file, or none if it is not there."""
    try:
        with open(path, errors='replace') as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


def diff_sections(lines):
    """Split regression.diffs into each test's lines, by its name."""
    out = defaultdict(list)
    name = None
    for line in lines:
        m = re.match(r'^\+\+\+ .*/results/(\S+)\.out', line)
        if m:
            name = m.group(1)
            continue
        if line.startswith('diff ') or line.startswith('--- '):
            continue
        if name:
            out[name].append(line)
    return out


def first_new_error(section):
    """The first error the diff adds, a warning if none, or None."""
    for kind in ('ERROR|FATAL|PANIC', 'WARNING'):
        for line in section:
            m = re.match(r'^\+(?:psql:[^:]*:\d+: )?(?:%s):\s+(.*)' % kind,
                         line)
            if m:
                return line[1:].strip(), m.group(1).strip()
    return None, None


def statements(lines):
    """Each line of psql's output, with the statement it belongs to: one
    starts at a line that begins with a word once the last has ended."""
    stmt = ''
    open_ = False
    for line in lines:
        if re.match(r'^(?:psql:[^:]*:\d+: )?(ERROR|DETAIL|HINT|CONTEXT|QUERY'
                    r'|LINE \d+|WARNING|NOTICE|server closed)', line):
            open_ = False
        elif not open_ and re.match(r'^[A-Za-z\\]', line):
            stmt = line[:100]
            open_ = not line.startswith('\\')
        if open_ and re.search(r';\s*(--.*)?$', line):
            open_ = False
        yield stmt, line


def setup_errors(lines):
    """Each statement of test_setup's output that failed, with its error."""
    out = []
    for stmt, line in statements(lines):
        m = re.match(r'^(?:psql:[^:]*:\d+: )?ERROR:\s+(.*)', line)
        if m:
            out.append((stmt, m.group(1)))
    return out


def crash_statement(lines):
    """The statement running when the server went away, or None."""
    for stmt, line in statements(lines):
        if line.startswith('server closed the connection'):
            return stmt
    return None


def error_statement(lines, msg):
    """The statement that raised the error msg first, or ''."""
    for stmt, line in statements(lines):
        if re.match(r'^(?:psql:[^:]*:\d+: )?(ERROR|FATAL|PANIC|WARNING):\s+'
                    + re.escape(msg) + '$', line):
            return stmt
    return ''


def setup_missing(errors):
    """The tables test_setup could not make, and the error that stopped
    each: for one made from another that is missing, that one's."""
    why = {}
    for stmt, err in errors:
        m = re.match(r'^CREATE TABLE (\w+)', stmt, re.I)
        if m:
            why.setdefault(m.group(1).lower(), err)
    for _ in range(len(why)):
        for t, err in why.items():
            m = re.match(r'^relation "(\w+)" does not exist$', err)
            if m and m.group(1) in why and m.group(1) != t:
                why[t] = why[m.group(1)]
    return why


def main():
    work, files = sys.argv[1], sys.argv[2:]
    rows = []
    causes = defaultdict(list)
    crashes = []
    setup = defaultdict(list)
    # Each cause's messages as they were, before normalize().
    raws = defaultdict(lambda: defaultdict(int))
    for t in files:
        d = os.path.join(work, t)
        status = (read(os.path.join(d, 'status')) or ['missing'])[0]
        sections = diff_sections(read(os.path.join(d, 'regression.diffs')))
        sec = sections.get(t, [])
        hunks = sum(1 for line in sec if line.startswith('@@'))
        raw, msg = first_new_error(sec)
        if raw:
            # An address differs from run to run, and says nothing.
            raw = re.sub(r'\b0x[0-9a-f]+\b', '0x?', raw)
            msg = re.sub(r'\b0x[0-9a-f]+\b', '0x?', msg)
        errors = setup_errors(
            read(os.path.join(d, 'results', 'test_setup.out')))
        for stmt, err in errors:
            setup[(stmt, err)].append(t)
        missing = setup_missing(errors)
        detail = msg
        results = read(os.path.join(d, 'results', t + '.out'))
        stmt = error_statement(results, msg) if msg else ''
        if status == 'crash':
            log = read(os.path.join(d, 'server.log'))
            crashes.append((t, crash_statement(results),
                [line for line in log if CRASH.search(line)]
                or ['(the server stopped; nothing in its log)']))
            cause = '(crash)'
            detail = re.sub(r', PID: \d+', '', crashes[-1][2][0])
        elif status == 'timeout':
            cause = '(timeout: still running after 180 s)'
        elif status == 'pass':
            cause = None
        elif msg is None:
            cause = '(results differ; no new error)'
        else:
            cause = normalize(msg)
            # A table test_setup did not make is missing for the reason
            # test_setup could not make it.
            m = re.match(r'^relation "(\w+)" does not exist$', msg)
            if m and m.group(1) in missing:
                cause = ('test_setup could not make a table: '
                         + normalize(missing[m.group(1)]))
                detail = '%s: %s' % (m.group(1), missing[m.group(1)])
        if cause:
            causes[cause].append(t)
            raws[cause][detail or cause] += 1
        rows.append((t, status, hunks, raw or '', stmt))

    with open(os.path.join(work, 'summary'), 'w') as f:
        for t, status, hunks, raw, stmt in rows:
            f.write('%-28s %-7s %4d  %s%s\n' % (t, status, hunks, raw,
                                                '  <- ' + stmt if stmt else ''))
        counts = defaultdict(int)
        for r in rows:
            counts[r[1]] += 1
        f.write('\n%d files: %s\n' % (len(rows), ', '.join(
            '%d %s' % (counts[s], s)
            for s in ('pass', 'fail', 'timeout', 'crash') if counts[s])))

    # A crash is a bug, not a missing feature: it comes first.
    order = sorted(causes, key=lambda c: (c != '(crash)', -len(causes[c]), c))
    with open(os.path.join(work, 'causes'), 'w') as f:
        for c in order:
            f.write('%4d  %s\n' % (len(causes[c]), c))
            f.write('      %s\n' % ' '.join(causes[c]))
            if len(raws[c]) > 1 or c not in raws[c]:
                for r, k in sorted(raws[c].items(), key=lambda kv: -kv[1]):
                    f.write('      %4d  %s\n' % (k, r))
            f.write('\n')

    with open(os.path.join(work, 'crashes'), 'w') as f:
        for t, stmt, lines in crashes:
            f.write('%s: %s\n' % (t, stmt or '(statement not found)'))
            for line in lines[:5]:
                f.write('    %s\n' % line)

    with open(os.path.join(work, 'setup'), 'w') as f:
        for (stmt, err), ts in setup.items():
            f.write('%4d files  %s\n            ERROR: %s\n'
                    % (len(ts), stmt, err))

    print('regress: %d files: %s; see %s/summary and %s/causes' % (
        len(rows), ', '.join('%d %s' % (counts[s], s)
                             for s in ('pass', 'fail', 'timeout', 'crash')
                             if counts[s]), work, work))
    for t, stmt, lines in crashes:
        print('regress: CRASH in %s: %s' % (t, lines[0]))


if __name__ == '__main__':
    main()
