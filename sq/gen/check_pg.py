#!/usr/bin/env python3
"""check_pg.py -- check spec/ and the generator against PostgreSQL in ../dg.

    gen/check_pg.py DG_SRC     DG_SRC is ../dg/src

This holds only while PostgreSQL's parser is still in ../dg (PLAN.md
step 6): it is the oracle's check on the generator.

1. The headers in spec/ are ../dg's, byte for byte.
2. spec/nodes.list names every struct the grammar can make.
3. For every struct the generated printer prints, the fields, their
   order and their kinds are those of PostgreSQL's generated printer.
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import gen_nodes  # noqa: E402

# How PostgreSQL's WRITE_x_FIELD names each kind of field.
PG_KIND = {
    "list": "NODE",
    "node": "NODE",
    "typed": "NODE",
    "string": "STRING",
    "bool": "BOOL",
    "char": "CHAR",
    "int": "INT",
    "uint": "UINT",
    "int64": "INT64",
    "long": "LONG",
    "location": "LOCATION",
    "enum": "ENUM",
}


def check_headers(dg):
    """Compare each spec header with the one in ../dg."""
    bad = 0
    for h in gen_nodes.HEADERS:
        ours = (gen_nodes.ROOT / "spec" / h).read_bytes()
        if ours != (dg / "include" / "nodes" / h).read_bytes():
            print(f"spec/{h} differs from ../dg's")
            bad += 1
    return bad


def grammar_structs(dg, spec):
    """The structs the grammar can make, and all they point at."""
    grammar = (dg / "backend/parser/gram.y").read_text()
    grammar += (dg / "backend/parser/parser.c").read_text()
    makefuncs = (dg / "backend/nodes/makefuncs.c").read_text()
    todo = set(re.findall(r"makeNode\((\w+)\)", grammar))
    for fn in set(re.findall(r"\b(make\w+)\(", grammar)):
        body = re.search(rf"\b{fn}\(.*?^}}", makefuncs, flags=re.M | re.S)
        if body:
            todo |= set(re.findall(r"makeNode\((\w+)\)", body.group(0)))
    todo |= {"Integer", "Float", "Boolean", "String", "BitString"}
    done = set()
    while todo:
        name = todo.pop()
        if name in done or name in gen_nodes.ANALYZER_ONLY or name == "Node":
            continue
        done.add(name)
        for f in spec.structs[name].fields:
            t = f.ctype.replace("struct ", "").replace("const ", "")
            t = t.replace("*", "").strip()
            if t in spec.structs:
                todo.add(t)
    return done


def check_list(dg, model):
    """Compare spec/nodes.list with what the grammar can make."""
    want = grammar_structs(dg, model.spec)
    have = model.names | {"Expr"}
    for name in sorted(want - have):
        print(f"spec/nodes.list lacks {name}, which the grammar makes")
    for name in sorted(have - want):
        print(f"spec/nodes.list has {name}, which the grammar cannot make")
    return len(want ^ have)


def check_fields(dg, model):
    """Compare each printed struct's fields with PostgreSQL's printer."""
    text = (dg / "backend/nodes/outfuncs.funcs.c").read_text()
    theirs = {}
    for m in re.finditer(
        r"static void\s+_out(\w+)\(.*?\{(.*?)^\}", text, flags=re.S | re.M
    ):
        fields = re.findall(r"WRITE_(\w+?)_FIELD\(([\w.]+)", m.group(2))
        # An Oid is written as an unsigned number, as a uint is.
        theirs[m.group(1)] = [("UINT" if k == "OID" else k, n) for k, n in fields]

    def walk(struct, prefix):
        out = []
        for kind, f in model.fields(struct):
            if kind == "embedded":
                out += walk(f.ctype, prefix + f.name + ".")
            else:
                out.append((PG_KIND[kind], prefix + f.name))
        return out

    bad = 0
    for s in model.structs:
        if s in gen_nodes.CUSTOM_PRINT:
            continue
        if theirs.get(s) != walk(s, ""):
            print(f"{s}: fields differ from PostgreSQL's printer")
            bad += 1
    return bad


def main():
    """Run the three checks; exit 1 if any found a difference."""
    if len(sys.argv) != 2:
        sys.exit("usage: check_pg.py DG_SRC")
    dg = Path(sys.argv[1])
    model = gen_nodes.model_t()
    bad = check_headers(dg) + check_list(dg, model) + check_fields(dg, model)
    printed = len(model.structs) - len(gen_nodes.CUSTOM_PRINT)
    print(f"spec: {len(model.structs)} structs, {printed} printed by the generator")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
