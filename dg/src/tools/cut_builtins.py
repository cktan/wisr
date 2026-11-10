#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

"""
cut_builtins.py -- take built-in functions out of the catalog data.

The built-ins are rows of src/include/catalog/*.dat. When the C
function behind a pg_proc row is gone, the row has to go, and so
does every row that names it: the operator it implements, the
operator class entries for that operator, the access method it is
the handler of, and on from there.

    cut_builtins.py FILE     run in dg/. FILE lists C function
                             names, one a line; each pg_proc row
                             with that prosrc is removed, then
                             whatever no longer resolves
    cut_builtins.py --language NAME
                             the same, for every pg_proc row
                             written in the language NAME, such as
                             sql: a function with no C function at
                             all

It does not work out what refers to what. genbki.pl does, and
says "unresolved OID reference" for each row left dangling, so
this removes the rows genbki.pl names and runs it again until it
has nothing to say.

A dangling reference removes its row, except in the few columns
listed in OPTIONAL, where the reference is an extra the row lives
without: there the column goes back to its default.

What was removed is printed, a row a line.
"""
import glob
import re
import subprocess
import sys

CATALOG = "src/include/catalog"
# (catalog, column) pairs whose reference is dropped, not the row.
OPTIONAL = {
    ("pg_type", "typanalyze"),
    ("pg_operator", "oprcom"),
    ("pg_operator", "oprnegate"),
    ("pg_operator", "oprrest"),
    ("pg_operator", "oprjoin"),
    ("pg_proc", "prosupport"),
}
UNRESOLVED = re.compile(r'unresolved OID reference "([^"]*)" in (\w+)\.dat '
                        r'field (\w+) line (\d+)')


def entries(lines):
    """Yield (first, last) line indexes of each { ... } row in lines."""
    start = None
    depth = 0
    for i, line in enumerate(lines):
        if start is None:
            if not line.startswith("{"):
                continue
            start = i
            depth = 0
        depth += line.count("{") - line.count("}")
        if depth == 0:
            yield start, i
            start = None


def describe(text):
    """Return the columns of a row that say what it is."""
    keys = ("oid", "proname", "oprname", "typname", "amname", "opfname",
            "opcname", "amopfamily", "amprocfamily", "amopopr", "amproc",
            "aggfnoid", "castsource", "casttarget", "prosrc")
    found = []
    for key in keys:
        m = re.search(rf"\b{key} => '([^']*)'", text)
        if m:
            found.append(f"{key}={m.group(1)}")
    return " ".join(found)


def remove_rows(catalog, doomed, why):
    """Remove from catalog.dat the rows that doomed(text) picks."""
    path = f"{CATALOG}/{catalog}.dat"
    with open(path) as f:
        lines = f.read().split("\n")
    out = []
    pos = 0
    count = 0
    for first, last in entries(lines):
        text = "\n".join(lines[first:last + 1])
        reason = doomed(text, last + 1)
        if reason is None:
            continue
        if isinstance(reason, str):
            # A replacement for the row, with a column dropped.
            out.extend(lines[pos:first])
            out.extend(reason.split("\n"))
        else:
            out.extend(lines[pos:first])
            # The blank line after a row goes with it.
            if last + 1 < len(lines) and lines[last + 1] == "":
                last += 1
            print(f"{catalog}: {describe(text)}  [{why}]")
            count += 1
        pos = last + 1
    out.extend(lines[pos:])
    with open(path, "w") as f:
        f.write("\n".join(out))
    return count


def genbki():
    """Run genbki.pl and return what it could not resolve."""
    headers = sorted(h for h in glob.glob(f"{CATALOG}/pg_*.h")
                     if not h.endswith("_d.h"))
    proc = subprocess.run(
        ["perl", "-Isrc/backend/catalog", "src/backend/catalog/genbki.pl",
         "--include-path=src/include/", "--set-version=18",
         f"--output={CATALOG}/"] + headers,
        capture_output=True, text=True)
    found = UNRESOLVED.findall(proc.stderr)
    if proc.returncode != 0 and not found:
        sys.exit(proc.stderr)
    return found


def main(listing, language=None):
    """Remove the chosen functions' rows, then every dangling row."""
    if language is not None:
        removed = remove_rows(
            "pg_proc",
            lambda text, _: True if re.search(
                rf"\bprolang => '{language}'", text) else None,
            f"language {language}")
    else:
        with open(listing) as f:
            gone = set(f.read().split())
        removed = remove_rows(
            "pg_proc",
            lambda text, _: True if set(
                re.findall(r"prosrc => '(\w+)'", text)) & gone else None,
            "no C function")
    while True:
        unresolved = genbki()
        if not unresolved:
            break
        by_catalog = {}
        for name, catalog, field, line in unresolved:
            by_catalog.setdefault(catalog, {}).setdefault(
                int(line), (name, field))
        for catalog, at in by_catalog.items():
            def doomed(text, last, at=at, catalog=catalog):
                if last not in at:
                    return None
                name, field = at[last]
                if (catalog, field) in OPTIONAL:
                    print(f"{catalog}: {describe(text)}  "
                          f"[{field} {name} dropped]")
                    return re.sub(rf",?\s*\b{field} => '[^']*'", "", text,
                                  count=1)
                return True
            removed += remove_rows(catalog, doomed, "dangling")
    print(f"{removed} rows removed")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--language":
        main(None, sys.argv[2])
    elif len(sys.argv) == 2:
        main(sys.argv[1])
    else:
        sys.exit(__doc__)
