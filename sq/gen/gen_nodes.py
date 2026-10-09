#!/usr/bin/env python3
"""gen_nodes.py -- write sq's node header and printer from PostgreSQL's.

    gen/gen_nodes.py header > include/sq_nodes.h
    gen/gen_nodes.py printer > build/gen/print_nodes.cc

The input is spec/: PostgreSQL 18.6's own node headers, unedited, and
nodes.list, which names the structs the parser can build. Porting to a
later PostgreSQL is replacing those headers and running this again.

What changes on the way from PostgreSQL's struct to sq's:

  - names: SelectStmt is sq_select_stmt_t, targetList is target_list,
    JoinType is sq_join_type_t and JOIN_INNER is SQ_JOIN_INNER;
  - a List * is a std::vector<sq_node_t*>, empty where PostgreSQL has
    NIL;
  - a char * is a const char *, owned by the tree, NULL where
    PostgreSQL's is;
  - every node derives from sq_node_t, which holds the tag;
  - A_Const holds a pointer to its value node, not a union of them.

The printer writes PostgreSQL's names, not sq's: its text is
nodeToString()'s, byte for byte.
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from nodespec import read_spec  # noqa: E402

ROOT = Path(__file__).parent.parent
HEADERS = ["nodes.h", "lockoptions.h", "value.h", "primnodes.h", "parsenodes.h"]

# Structs only the analyzer makes. A field pointing at one is kept, as
# a plain node pointer the parser leaves NULL.
ANALYZER_ONLY = {"Query", "FuncExpr"}

# Printed by hand in src/print.cc, as PostgreSQL prints them by hand.
CUSTOM_PRINT = {
    "A_Const",
    "A_Expr",
    "BoolExpr",
    "Integer",
    "Float",
    "Boolean",
    "String",
    "BitString",
}

# PostgreSQL's scalar types: the C++ type, and how nodeToString()
# formats one.
SCALARS = {
    "bool": ("bool", "bool"),
    "char": ("char", "char"),
    "int": ("int", "int"),
    "int16": ("int16_t", "int"),
    "int32": ("int32_t", "int"),
    "int64": ("int64_t", "int64"),
    "long": ("long", "long"),
    "Index": ("uint32_t", "uint"),
    "bits32": ("uint32_t", "uint"),
    "SubTransactionId": ("uint32_t", "uint"),
    "Oid": ("uint32_t", "uint"),
    "RelFileNumber": ("uint32_t", "uint"),
    "ParseLoc": ("int", "location"),
}


def snake(name):
    """Turn PostgreSQL's CamelCase or camelCase into snake_case."""
    name = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", name)
    name = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", name)
    return name.lower()


def type_name(name):
    """The sq type for a PostgreSQL struct or enum."""
    return f"sq_{snake(name)}_t"


def tag_name(name):
    """The sq node tag for a PostgreSQL struct."""
    return f"SQ_T_{snake(name).upper()}"


class model_t:
    """The spec, narrowed to what sq emits, with fields classified."""

    def __init__(self):
        """Read spec/ and work out the structs and enums to emit."""
        self.spec = read_spec(str(ROOT / "spec"), HEADERS)
        listed = [
            line.strip()
            for line in open(ROOT / "spec" / "nodes.list")
            if line.strip() and not line.startswith("#")
        ]
        for name in listed:
            if name not in self.spec.structs:
                sys.exit(f"nodes.list: {name} is not a struct in spec/")
        # Expr is PostgreSQL's formal base of expression nodes: it holds
        # the tag and nothing else, which sq_node_t already does.
        self.structs = [n for n in self.spec.structs if n in listed and n != "Expr"]
        self.names = set(self.structs)
        self.enums = []
        for s in self.structs:
            for kind, f in self.fields(s):
                if kind == "enum" and f.ctype not in self.enums:
                    self.enums.append(f.ctype)
        self.members = {
            m for e in self.spec.enums.values() for m, _ in e.members
        }

    def classify(self, struct, f):
        """Say what kind of field f is: how it is typed and printed."""
        t = f.ctype.replace("struct ", "").replace("const ", "").strip()
        if t == "List *":
            return "list"
        if t == "char *":
            return "string"
        if t in ("Node *", "Expr *"):
            return "node"
        if t.endswith(" *"):
            base = t[:-2]
            if base in self.names:
                return "typed"
            if base in ANALYZER_ONLY:
                return "node"
        elif t in SCALARS:
            return SCALARS[t][1]
        elif t in self.spec.enums:
            return "enum"
        elif t in self.names:
            return "embedded"
        sys.exit(f"{struct}.{f.name}: no rule for type {f.ctype!r}")

    def fields(self, struct):
        """The fields of struct to emit, as (kind, field), without the tag."""
        out = []
        for i, f in enumerate(self.spec.structs[struct].fields):
            if i == 0 and f.ctype in ("NodeTag", "Expr"):
                continue
            if struct == "A_Const" and f.ctype == "union ValUnion":
                continue
            out.append((self.classify(struct, f), f))
        return out

    def cxx_type(self, kind, f):
        """The C++ type of a field of the given kind."""
        t = f.ctype.replace("struct ", "").replace("const ", "").strip()
        if kind == "list":
            return "std::vector<sq_node_t*>"
        if kind == "string":
            return "const char*"
        if kind == "node":
            return "sq_node_t*"
        if kind == "typed":
            return type_name(t[:-2]) + "*"
        if kind in ("enum", "embedded"):
            return type_name(t)
        return SCALARS[t][0]


# ---- the header ----

HEADER_TOP = """\
/*
 * sq_nodes.h -- the nodes of an sq parse tree.
 *
 * GENERATED by gen/gen_nodes.py from PostgreSQL's node headers in
 * spec/. Do not edit: change the generator, and run `make nodes`.
 *
 * Each struct is PostgreSQL's of the same name, renamed: SelectStmt
 * is sq_select_stmt_t and its targetList is target_list. What a
 * field means is what PostgreSQL's means, and is documented in
 * spec/parsenodes.h and spec/primnodes.h.
 *
 *   - Every node derives from sq_node_t and sets its own tag when
 *     made. A node is told apart by its tag, and cast.
 *   - A list is a std::vector<sq_node_t*>. Where a list stands in a
 *     place that holds any node -- an element of another list, or a
 *     field typed sq_node_t* -- it is an sq_list_t.
 *   - Text is a const char*, NUL-terminated and owned by the tree.
 *     NULL means absent, which is not the same as empty.
 *   - A node pointer is NULL where the thing is absent.
 *   - A location is a byte offset into the text parsed, or -1.
 *   - An enum's values are PostgreSQL's, with SQ_ before each name.
 */
#ifndef SQ_NODES_H
#define SQ_NODES_H

#include <cstdint>
#include <vector>
"""


def enum_value(model, value):
    """An enumerator's explicit value, with sq's names for PostgreSQL's."""
    return re.sub(
        r"\b[A-Za-z_]\w*\b",
        lambda m: "SQ_" + m.group(0) if m.group(0) in model.members else m.group(0),
        value,
    )


def default_of(kind):
    """The initializer that gives a field the value palloc0 would."""
    if kind in ("list", "embedded"):
        return ""
    if kind in ("string", "node", "typed"):
        return " = nullptr"
    if kind == "bool":
        return " = false"
    if kind == "enum":
        return "{}"
    return " = 0"


def write_header(model, out):
    """Write include/sq_nodes.h."""
    w = out.write
    w(HEADER_TOP)

    w("\n/* What a node is: one tag for each struct below. */\n")
    w("enum sq_nodetag_t {\n    SQ_T_INVALID = 0,\n    SQ_T_LIST,\n")
    for s in model.structs:
        w(f"    {tag_name(s)},\n")
    w("};\n")

    for e in model.enums:
        w(f"\n/* PostgreSQL's {e}. */\nenum {type_name(e)} {{\n")
        for name, value in model.spec.enums[e].members:
            w(f"    SQ_{name}")
            if value is not None:
                w(f" = {enum_value(model, value)}")
            w(",\n")
        w("};\n")

    w("\n/* What every node begins with. */\n")
    w("struct sq_node_t {\n    sq_nodetag_t tag = SQ_T_INVALID;\n};\n")
    w("\n/* A list, where a node is expected: PostgreSQL's List. */\n")
    w("struct sq_list_t : sq_node_t {\n")
    w("    std::vector<sq_node_t*> items;\n\n")
    w("    sq_list_t() { tag = SQ_T_LIST; }\n};\n\n")

    for s in model.structs:
        w(f"struct {type_name(s)};\n")

    for s in model.structs:
        header = model.spec.structs[s].header
        w(f"\n/* PostgreSQL's {s} ({header}). */\n")
        w(f"struct {type_name(s)} : sq_node_t {{\n")
        if s == "A_Const":
            w("    /* An Integer, Float, Boolean, String or BitString node. */\n")
            w("    sq_node_t* val = nullptr;\n")
        for kind, f in model.fields(s):
            if f.ctype.replace("struct ", "")[:-2] in ANALYZER_ONLY:
                w("    /* The analyzer's to fill; NULL from the parser. */\n")
            w(f"    {model.cxx_type(kind, f)} {snake(f.name)}{default_of(kind)};\n")
        w(f"\n    {type_name(s)}() {{ tag = {tag_name(s)}; }}\n}};\n")

    w("\n#endif /* SQ_NODES_H */\n")


# ---- the printer ----

PRINTER_TOP = """\
/*
 * print_nodes.cc -- write a node's label and fields, as PostgreSQL's
 * nodeToString() does.
 *
 * GENERATED by gen/gen_nodes.py from PostgreSQL's node headers in
 * spec/. Do not edit: change the generator.
 */

#include "print.h"

namespace sq {
"""


def print_calls(model, struct, access, prefix, out):
    """Write the calls that print struct's fields, read through access."""
    for kind, f in model.fields(struct):
        label = prefix + f.name
        member = access + snake(f.name)
        if kind == "embedded":
            print_calls(model, f.ctype, member + ".", label + ".", out)
            continue
        out.write(f'  s += " :{label} ";\n')
        if kind == "location":
            # nodeToString() hides locations, so that trees compare equal.
            out.write('  s += "-1";\n')
        elif kind == "enum":
            out.write(f"  out_int(s, static_cast<int>({member}));\n")
        else:
            out.write(f"  out_{kind}(s, {member});\n")


def write_printer(model, out):
    """Write build/gen/print_nodes.cc."""
    w = out.write
    w(PRINTER_TOP)
    generic = [s for s in model.structs if s not in CUSTOM_PRINT]
    for s in generic:
        w(f"\n/* Write a {s}. */\n")
        w(f"static void out_{snake(s)}(std::string &s, const {type_name(s)} &n) {{\n")
        w(f'  s += "{s.upper()}";\n')
        if not model.fields(s):
            w("  (void)n;\n")
        print_calls(model, s, "n.", "", out)
        w("}\n")

    w("\n/* Write the label and fields of a node no one prints by hand. */\n")
    w("bool out_generated(std::string &s, const sq_node_t *node) {\n")
    w("  switch (node->tag) {\n")
    for s in generic:
        w(f"  case {tag_name(s)}:\n")
        w(f"    out_{snake(s)}(s, *static_cast<const {type_name(s)} *>(node));\n")
        w("    return true;\n")
    w("  default:\n    return false;\n  }\n}\n\n} // namespace sq\n")


def write_fields(model, out):
    """List each printed struct's fields, for checking against PostgreSQL."""

    def walk(struct, prefix):
        for kind, f in model.fields(struct):
            if kind == "embedded":
                walk(f.ctype, prefix + f.name + ".")
            else:
                out.write(f" {kind}:{prefix}{f.name}")

    for s in model.structs:
        if s not in CUSTOM_PRINT:
            out.write(s)
            walk(s, "")
            out.write("\n")


def main():
    """Write the file named on the command line to standard output."""
    writers = {"header": write_header, "printer": write_printer, "fields": write_fields}
    if len(sys.argv) != 2 or sys.argv[1] not in writers:
        sys.exit("usage: gen_nodes.py header|printer|fields")
    writers[sys.argv[1]](model_t(), sys.stdout)


if __name__ == "__main__":
    main()
