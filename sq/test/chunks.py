#!/usr/bin/env python3
"""chunks.py -- cut psql scripts into pieces a parser can be given.

    test/chunks.py DIR OUT     DIR holds *.sql; OUT gets FILE/NNNNN.sql

PostgreSQL's regression SQL is written for psql: it has backslash
commands, rows of COPY data, and statements that are wrong on purpose.
The oracle cannot split such a file into statements, because
PostgreSQL's parser returns nothing for text with an error in it
(PLAN.md step 0). This cuts one a cruder way, which is enough to
compare two parsers by:

1. Drop the lines that begin with a backslash: psql's own commands.
2. Scan what is left with `oracle --tokens`.
3. Cut after each semicolon that is outside parentheses.

A piece need not be a statement, or even parse: the rows after a COPY
are a piece, and so is a statement that is wrong on purpose. What
matters is that sq and PostgreSQL say the same of each, whatever it is.
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).parent.parent


def cut(text):
    """Cut text after each top-level semicolon PostgreSQL's scanner sees."""
    run = subprocess.run(
        [str(ROOT / "build" / "oracle"), "--tokens"],
        input=text,
        capture_output=True,
    )
    pieces, start, depth = [], 0, 0
    for line in run.stdout.split(b"\n"):
        parts = line.split(b" ", 2)
        # A token's line begins with its number; an error's does not.
        if len(parts) < 2 or not parts[0].isdigit():
            continue
        token, at = int(parts[0]), int(parts[1])
        if token == ord("("):
            depth += 1
        elif token == ord(")"):
            depth = max(0, depth - 1)
        elif token == ord(";") and depth == 0:
            pieces.append(text[start : at + 1])
            start = at + 1
    if text[start:].strip():
        pieces.append(text[start:])
    return pieces


def main():
    """Cut every file of DIR into OUT."""
    if len(sys.argv) != 3:
        sys.exit("usage: chunks.py DIR OUT")
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    total = 0
    for path in sorted(src.glob("*.sql")):
        lines = path.read_bytes().split(b"\n")
        text = b"\n".join(l for l in lines if not l.startswith(b"\\"))
        pieces = cut(text)
        where = out / path.stem
        where.mkdir(parents=True, exist_ok=True)
        for n, piece in enumerate(pieces, 1):
            (where / f"{n:05d}.sql").write_bytes(piece + b"\n")
        total += len(pieces)
    print(f"chunks: {total} pieces from {src}")


if __name__ == "__main__":
    main()
