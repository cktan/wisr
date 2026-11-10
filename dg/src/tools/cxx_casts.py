#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

"""
cxx_casts.py -- write the casts C leaves out and C++ requires.

C converts a void pointer to any object pointer, an int to an enum
and a const char pointer from strchr to a char pointer without
being asked. C++ refuses each one: g++ calls it an "invalid
conversion", and only -fpermissive lets it through. This reads
what g++ said and writes the cast, (T) in front of the expression,
which is what C did silently.

It works from g++'s own diagnostics, so it never guesses a type
or where an expression ends:

    cxx_casts.py --cxx g++ ARGS...     run as the compiler: compile
                                       with ARGS and keep the
                                       diagnostics beside the object,
                                       in x.o.diag
    cxx_casts.py --apply DIR           write the casts every x.o.diag
                                       under DIR asks for, and remove
                                       the x.o.diag files

So one pass over dg/ is

    make -C src -B -k -j4 STD='-x c++ -std=gnu++20 -w -fsyntax-only' \\
        CXX='python3 $(top)/src/tools/cxx_casts.py --cxx g++'
    python3 src/tools/cxx_casts.py --apply src

and a cast can uncover the next conversion inside the same
expression, so it is repeated until a pass writes nothing.

A conversion inside a macro's body is reported where the macro
is used. A cast there is right when the macro is the expression,
as lfirst() is, and wrong when the conversion is deeper inside.
g++ does not say which, so the cast is tried, and taken out again
on the next pass if the line still has an error. .cxx_casts under
DIR remembers the casts on trial and the macros found wrong, and
is to be removed when the passes are done.

A place is left alone, and listed, when two compilations want
different types there, when the conversion is inside a macro, or
when the type has no name to cast to.
"""
import json
import os
import re
import subprocess
import sys

CONVERSION = re.compile(r"^invalid conversion from ‘[^’]*’(?: \{aka ‘[^’]*’\})?"
                        r" to ‘([^’]*)’")
# Binary and ternary operators: a cast in front of an expression
# with one of these outside every bracket would bind to its first
# operand only.
BINARY = re.compile(r"[-+*/%&|^?:<>=!,]")
WORD = re.compile(rb"[A-Za-z_][A-Za-z0-9_]*")
# Between passes: the casts on trial in front of a macro, and the
# macros where one was tried and was wrong.
STATE = ".cxx_casts"


def compile_keeping_diagnostics(argv):
    """Run the compiler, saving its JSON diagnostics as OBJECT.diag."""
    obj = argv[argv.index("-o") + 1]
    proc = subprocess.run(argv + ["-fdiagnostics-format=json"],
                          stderr=subprocess.PIPE)
    with open(obj + ".diag", "wb") as f:
        f.write(os.getcwd().encode() + b"\n" + proc.stderr)
    return proc.returncode


def needs_parens(expr):
    """Say whether expr has a binary operator outside every bracket."""
    depth = 0
    i = 0
    # Leading unary operators bind as tightly as the cast does.
    while i < len(expr) and expr[i] in "*&-!~+ ":
        i += 1
    while i < len(expr):
        c = expr[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c in "\"'":
            i += 1
            while i < len(expr) and expr[i] != c:
                i += 2 if expr[i] == "\\" else 1
        elif depth == 0 and expr.startswith("->", i):
            i += 1
        elif depth == 0 and BINARY.match(c):
            return True
        i += 1
    return False


def closing_paren(data, i):
    """Return the offset of the parenthesis closing the one at i."""
    depth = 0
    while True:
        c = chr(data[i])
        if c in "\"'":
            i += 1
            while chr(data[i]) != c:
                i += 2 if chr(data[i]) == "\\" else 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1


def collect(root):
    """Read every .diag under root. Return the casts wanted, as
    {file: {(start, finish): set of types}}, the lines with any error,
    as {file: set of lines}, and the .diag files read."""
    wanted = {}
    errors = {}
    read = []
    for dirpath, _, names in os.walk(root):
        for name in names:
            if not name.endswith(".diag"):
                continue
            path = os.path.join(dirpath, name)
            with open(path, "rb") as f:
                cwd = f.readline().decode().rstrip("\n")
                text = f.read().decode(errors="replace")
            read.append(path)
            start = text.find("[")
            if start < 0:
                continue
            try:
                diags = json.loads(text[start:])
            except ValueError:
                print(f"unreadable diagnostics: {path}")
                continue
            for d in diags:
                if d["kind"] != "error" or not d["locations"]:
                    continue
                loc = d["locations"][0]
                first = loc.get("start", loc["caret"])
                last = loc.get("finish", loc["caret"])
                file = os.path.realpath(os.path.join(cwd, first["file"]))
                errors.setdefault(file, set()).add(first["line"])
                m = CONVERSION.match(d["message"])
                if not m or first["file"] != last["file"]:
                    continue
                key = ((first["line"], first["byte-column"]),
                       (last["line"], last["byte-column"]))
                wanted.setdefault(file, {}).setdefault(key, set()).add(
                    m.group(1))
    return wanted, errors, read


def apply(root):
    """Write the casts, and report what was written and what was not."""
    wanted, errors, read = collect(root)
    state_path = os.path.join(root, STATE)
    state = {"trial": [], "macro": []}
    if os.path.exists(state_path):
        with open(state_path) as f:
            state = json.load(f)
    macro = {tuple(k) for k in state["macro"]}
    written = 0
    skipped = []

    # A cast tried in front of a macro that left an error on its line
    # was wrong: the conversion is inside the macro. Take it out.
    undone = {}
    for file, line, col, cast, ident in state["trial"]:
        if line in errors.get(file, ()):
            undone.setdefault(file, []).append((line, col, cast))
            macro.add((file, line, ident))
    for file, casts in undone.items():
        with open(file, "rb") as f:
            lines = f.read().split(b"\n")
        for line, col, cast in sorted(casts, reverse=True):
            text = lines[line - 1]
            if text[col - 1:col - 1 + len(cast)] == cast.encode():
                lines[line - 1] = text[:col - 1] + text[col - 1 + len(cast):]
        with open(file, "wb") as f:
            f.write(b"\n".join(lines))
    trials = []

    for file, places in sorted(wanted.items()):
        if not os.path.exists(file):
            # Named by a #line in a generated file, from another directory.
            skipped.append(f"{file}: not found")
            continue
        ignored = subprocess.run(["git", "check-ignore", "-q", file],
                                 cwd=os.path.dirname(file)).returncode == 0
        touched = {line for line, _, _ in undone.get(file, ())}
        with open(file, "rb") as f:
            lines = f.read().split(b"\n")
        offsets = [0]
        for line in lines:
            offsets.append(offsets[-1] + len(line) + 1)
        data = bytearray(b"\n".join(lines))
        edits = []
        for (first, last), types in places.items():
            typ = next(iter(types))
            where = f"{file}:{first[0]}"
            if first[0] in touched:
                # Its columns moved when the cast came out; next pass.
                continue
            if ignored:
                skipped.append(f"{where}: generated file")
                continue
            if len(types) > 1:
                skipped.append(f"{where}: {' or '.join(sorted(types))}")
                continue
            if "<" in typ or "(" in typ and "(*" not in typ:
                skipped.append(f"{where}: no name for {typ}")
                continue
            if file.endswith((".y", ".l")):
                # Columns are those of the generated C, where $1 and
                # yytext are spelled out longer.
                skipped.append(f"{where}: grammar or scanner, wants {typ}")
                continue
            a = offsets[first[0] - 1] + first[1] - 1
            b = offsets[last[0] - 1] + last[1]
            # An expression that ends in a macro ends, for g++, where
            # the macro's name starts: take in the name and arguments.
            word = WORD.match(data, b - 1)
            if first != last and word and word.end() > b:
                b = word.end()
                if data[b:b + 1] == b"(":
                    b = closing_paren(data, b) + 1
            # g++ gives one column, not a range, for an expression that
            # comes out of a macro: the place the macro is used.
            ident = None
            if first == last:
                word = WORD.match(data, a)
                if word and word.end() > b:
                    ident = word.group().decode()
                    if (file, first[0], ident) in macro:
                        skipped.append(f"{where}: inside macro {ident}, "
                                       f"wants {typ}")
                        continue
            edits.append((a, b, typ, first, ident))
        # Innermost and last first, so earlier offsets stay good; one
        # of two overlapping places waits for the next pass.
        edits.sort(key=lambda e: (-e[0], e[1]))
        limit = len(data) + 1
        done = []
        for a, b, typ, first, ident in edits:
            if b > limit:
                continue
            limit = a
            expr = data[a:b].decode(errors="replace")
            cast = f"({typ})".encode()
            if ident is None and needs_parens(expr):
                data[a:b] = cast + b"(" + data[a:b] + b")"
            else:
                data[a:b] = cast + data[a:b]
            done.append((a, len(cast), first, ident))
            written += 1
        for a, _, first, ident in done:
            if ident is None:
                continue
            # Where the cast is now: after those written to its left
            # on the same line.
            shift = sum(n for b, n, other, _ in done
                        if other[0] == first[0] and b < a)
            trials.append((file, first[0], first[1] + shift,
                           f"({places_type(wanted, file, first)})", ident))
        with open(file, "wb") as f:
            f.write(data)
    for path in read:
        os.remove(path)
    with open(state_path, "w") as f:
        json.dump({"trial": trials, "macro": sorted(macro)}, f)
    for line in sorted(set(skipped)):
        print("left alone:", line)
    print(f"{written} casts written, "
          f"{sum(len(c) for c in undone.values())} taken out, "
          f"{len(set(skipped))} places left alone")


def places_type(wanted, file, first):
    """Return the one type wanted at the single-column place first."""
    return next(iter(wanted[file][(first, first)]))


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--cxx":
        sys.exit(compile_keeping_diagnostics(sys.argv[2:]))
    elif len(sys.argv) == 3 and sys.argv[1] == "--apply":
        apply(sys.argv[2])
    else:
        sys.exit(__doc__)
