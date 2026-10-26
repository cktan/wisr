#!/usr/bin/env python3
# triage.py -- sort the --dg-differs statements that "pgregress.py differs"
# dumped, by what their outputs on PostgreSQL 18 and digres show
# (TRIAGE.md).
#
#   triage.py              each category's count, causes and files
#   triage.py CATEGORY     each statement of CATEGORY, its two outputs
import glob
import json
import os
import re
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'regress'))
from report import normalize  # noqa: E402

DUMP = os.path.join(HERE, '..', '..', 'build', 'test', 'pgregress', 'differs')
# Lines that are a message, not a result.
MSG = re.compile(r'^(ERROR|WARNING|NOTICE|INFO|DETAIL|HINT|CONTEXT|LINE \d+|'
                 r'QUERY|\s*\^)')
# An error digres raises for a feature it lacks.
REFUSED = re.compile(r'not supported|digres|yet$')


def first_error(block):
    """The first error in a unit's output, or None."""
    for line in (block or '').split('\n'):
        m = re.match(r'^ERROR:\s+(.*)', line)
        if m:
            return m.group(1).strip()
    return None


def category(pg, dg):
    """What a unit's two outputs show: a category, and its cause."""
    if dg is None or pg is None:
        return 'lost', ''
    a, b = pg.split('\n'), dg.split('\n')
    if sorted(a) == sorted(b):
        return 'row order', ''
    pe, de = first_error(pg), first_error(dg)
    if 'QUERY PLAN' in pg:
        return 'plan', ''
    if pe and de:
        if normalize(pe) == normalize(de):
            return 'error detail', normalize(pe)
        if REFUSED.search(de):
            return 'error: digres refuses', normalize(de)
        return 'error: another', normalize(pe) + '  ->  ' + normalize(de)
    if pe:
        return 'accepts what PG rejects', normalize(pe)
    if de:
        return 'errors where PG does not', normalize(de)
    diff = [l for l in a if l not in b] + [l for l in b if l not in a]
    if all(MSG.match(l) for l in diff):
        msgs = [l for l in diff if not l.startswith(' ')]
        return 'notices only', normalize(msgs[0]) if msgs else ''
    return 'result', ''


def main():
    want = sys.argv[1] if len(sys.argv) > 1 else None
    cats = Counter()
    causes = defaultdict(Counter)
    files = defaultdict(Counter)
    for path in sorted(glob.glob(os.path.join(DUMP, '*.json'))):
        name = os.path.basename(path)[:-5]
        with open(path) as f:
            units = json.load(f)
        for it in units:
            c, cause = category(it['pg'], it['dg'])
            cats[c] += 1
            causes[c][cause] += 1
            files[c][name] += 1
            if c != want:
                continue
            a, b = (it['pg'] or '').split('\n'), (it['dg'] or '').split('\n')
            print('=' * 70)
            print('%s unit %d: %s' % (name, it['unit'], it['sql'][:300]))
            print('--- pg only:')
            print('\n'.join(l for l in a if l not in b)[:800])
            print('--- dg only:')
            print('\n'.join(l for l in b if l not in a)[:800])
    if want:
        return
    for c, n in cats.most_common():
        print('%5d  %s' % (n, c))
        for cause, m in causes[c].most_common(8):
            if cause:
                print('         %4d  %s' % (m, cause[:110]))
        print('         files: ' + ', '.join(
            '%s %d' % kv for kv in files[c].most_common(8)))


if __name__ == '__main__':
    main()
