#!/usr/bin/env python3
"""corpus.py -- check sq over PostgreSQL's regression SQL, by digest.

    test/corpus.py DIR                    compare with test/regress.digest
    test/corpus.py DIR --write            write test/regress.digest
    test/corpus.py DIR --tokens CMD --parse CMD --locations CMD
                                          the programs to run

DIR is PostgreSQL 18.6's src/test/regress/sql. For each script in it
this takes three digests:

  tokens     of what the scanner prints for the whole script: every
             token's number, where it begins and its value, or the
             error;
  pieces     of what the parser prints for each piece of the script,
             one after another: the trees, or the error with its
             position, detail and hint;
  locations  of the same trees printed whole, with where each
             statement was written and every location in it shown.

test/regress.digest holds all three for every script, as PostgreSQL's
own scanner and parser gave them: the first two were written from
them before sq stopped building them beside its own (PLAN.md step 6),
and the third from PostgreSQL's parser as it is in this repository's
history, built again for the purpose. A digest that differs names
the script; to see what differs, run the tools on its pieces in
build/pieces.

A script is cut into pieces a parser can be given, since it is written
for psql and has backslash commands, rows of COPY data, and statements
that are wrong on purpose:

1. Drop the lines that begin with a backslash: psql's own commands.
2. Scan what is left.
3. Cut after each semicolon that is outside parentheses.

A piece need not be a statement, or even parse. What matters is that
sq says of each what PostgreSQL said.
"""

import argparse
import hashlib
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).parent.parent
DIGESTS = ROOT / "test" / "regress.digest"


def run(cmd, text):
    """What cmd prints, given text on standard input."""
    return subprocess.run(cmd, input=text, capture_output=True).stdout


def cut(text, tokens):
    """Cut text after each top-level semicolon the scanner saw."""
    pieces, start, depth = [], 0, 0
    for line in tokens.split(b"\n"):
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


def digest_script(path, tokens_cmd, parse_cmd, locations_cmd, keep):
    """One script's line of the digest file."""
    whole = path.read_bytes()
    tokens = hashlib.sha256(run(tokens_cmd, whole)).hexdigest()

    lines = whole.split(b"\n")
    text = b"\n".join(l for l in lines if not l.startswith(b"\\"))
    pieces = cut(text, run(tokens_cmd, text))
    trees = hashlib.sha256()
    locations = hashlib.sha256()
    for n, piece in enumerate(pieces, 1):
        piece += b"\n"
        if keep:
            where = keep / path.stem
            where.mkdir(parents=True, exist_ok=True)
            (where / f"{n:05d}.sql").write_bytes(piece)
        trees.update(run(parse_cmd, piece))
        locations.update(run(locations_cmd, piece))
    return (
        f"{path.stem} {len(pieces)} {tokens} {trees.hexdigest()} "
        f"{locations.hexdigest()}"
    )


def main():
    """Digest every script of DIR, and write or compare the digests."""
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", type=Path)
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--tokens", default=str(ROOT / "build" / "sq_tokens"))
    ap.add_argument("--parse", default=str(ROOT / "build" / "sq_parse"))
    ap.add_argument("--locations", default=None)
    args = ap.parse_args()

    if args.locations is None:
        args.locations = args.parse + " --locations"
    scripts = sorted(args.dir.glob("*.sql"))
    if not scripts:
        sys.exit(f"corpus: no *.sql in {args.dir}")
    keep = None if args.write else ROOT / "build" / "pieces"
    with ThreadPoolExecutor(max_workers=4) as pool:
        got = list(
            pool.map(
                lambda p: digest_script(
                    p,
                    args.tokens.split(),
                    args.parse.split(),
                    args.locations.split(),
                    keep,
                ),
                scripts,
            )
        )
    pieces = sum(int(line.split()[1]) for line in got)

    if args.write:
        DIGESTS.write_text("\n".join(got) + "\n")
        print(f"corpus: wrote {len(got)} scripts, {pieces} pieces")
        return
    want = DIGESTS.read_text().splitlines()
    bad = sorted(set(got) ^ set(want))
    for name in sorted({line.split()[0] for line in bad}):
        print(f"corpus: {name} differs (its pieces are in build/pieces/{name})")
    if bad:
        sys.exit(f"corpus: FAILED ({len(want)} scripts expected)")
    print(f"corpus: PASSED ({len(got)} scripts, {pieces} pieces)")


if __name__ == "__main__":
    main()
