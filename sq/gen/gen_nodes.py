#!/usr/bin/env python3
"""gen_nodes.py -- write sq's node header and printer from PostgreSQL's.

    gen/gen_nodes.py header > include/sq_nodes.h
    gen/gen_nodes.py printer > build/gen/print_nodes.cc
    gen/gen_nodes.py funcs > build/gen/node_funcs.cc
    gen/gen_nodes.py converter > ../dg/src/backend/parser/sq_convert.c

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

node_funcs.cc has PostgreSQL's copyObject() and equal() for these
nodes, which the grammar calls: a deep copy into the tree being built,
and equality that ignores locations.

The converter is not sq's: it is written for digres, which compiles
it. It turns a tree of sq's nodes into PostgreSQL's own node structs,
which digres's analyzer reads, and so it names both. It is generated
here because it is the same walk over the same fields.
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

# Statements digres has no part of, which the converter refuses, by what
# it says digres does not support; digres has no node for them.
REFUSED = {
    "CreatePublicationStmt": "publications",
    "AlterPublicationStmt": "publications",
    "CreateSubscriptionStmt": "subscriptions",
    "AlterSubscriptionStmt": "subscriptions",
    "DropSubscriptionStmt": "subscriptions",
}

# Nodes found only under a refused statement: no converter for them.
UNDER_REFUSED = {"PublicationTable", "PublicationObjSpec"}

# Kinds of object digres has none of, refused wherever a statement names
# one (DROP, COMMENT, ALTER ... RENAME or OWNER, and the rest).
REFUSED_OBJECTS = {
    "OBJECT_PUBLICATION": "publications",
    "OBJECT_PUBLICATION_NAMESPACE": "publications",
    "OBJECT_PUBLICATION_REL": "publications",
    "OBJECT_SUBSCRIPTION": "subscriptions",
}

# Enums no node's field is typed with, whose values the grammar stores
# in an int field or passes between its rules.
EXTRA_ENUMS = ["SetQuantifier", "TableLikeOption"]

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
        self.enums = list(EXTRA_ENUMS)
        for s in self.structs:
            for kind, f in self.fields(s):
                if kind == "enum" and f.ctype not in self.enums:
                    self.enums.append(f.ctype)
        self.members = {
            m for e in self.spec.enums.values() for m, _ in e.members
        }
        self.constants = self.read_constants()

    def read_constants(self):
        """The constants parsenodes.h #defines for fields of its nodes."""
        text = (ROOT / "spec" / "parsenodes.h").read_text()
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        text = text.replace("\\\n", " ")
        out = []
        for m in re.finditer(r"^#define[ \t]+([A-Z]\w*)[ \t]+(.+?)[ \t]*$", text, flags=re.M):
            name, value = m.group(1), " ".join(m.group(2).split())
            # A constant is numbers, characters, operators, and the
            # constants before it; anything else is some other macro.
            words = re.findall(r"[A-Za-z_]\w*", re.sub(r"'.'|0x\w+", "", value))
            known = {n for n, _ in out} | {"LONG_MAX"}
            if re.fullmatch(r"[\w\s()|<~&+\-']+", value) and set(words) <= known:
                out.append((name, value))
        return out

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

    def all_fields(self, struct):
        """The members of sq's struct, as (kind, sq's name, attributes)."""
        out = []
        if struct == "A_Const":
            # sq holds the value node by pointer (see the header).
            out.append(("node", "val", []))
        for kind, f in self.fields(struct):
            out.append((kind, snake(f.name), f.attrs))
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

#include <climits>
#include <cstdint>
#include <vector>
"""


def enum_value(model, value):
    """An enumerator's explicit value, with sq's names for PostgreSQL's."""
    value = value.replace("PG_INT32_MAX", "INT32_MAX")
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

    w("\n/*\n * PostgreSQL's constants for fields of the nodes below, which are\n")
    w(" * #defines in its parsenodes.h.\n */\n")
    names = {n for n, _ in model.constants}
    for name, value in model.constants:
        value = re.sub(
            r"\b[A-Z]\w*\b",
            lambda m: "SQ_" + m.group(0) if m.group(0) in names else m.group(0),
            value,
        )
        ctype = "char" if re.fullmatch(r"'.'", value) else "long" if "LONG_MAX" in value else "int"
        w(f"inline constexpr {ctype} SQ_{name} = {value};\n")

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
        if kind == "enum":
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


FUNCS_TOP = """\
/*
 * node_funcs.cc -- copy_node and equal_nodes: PostgreSQL's
 * copyObject() and equal(), for the nodes of a parse tree.
 *
 * GENERATED by gen/gen_nodes.py from PostgreSQL's node headers in
 * spec/. Do not edit: change the generator.
 */

#include "nodefuncs.h"

#include <string.h>

#include "tree.h"

namespace sq {

/* Copy text into the tree being built; NULL stays NULL. */
static const char *copy_text(const char *text) {
  return text ? dup_string(*current_tree(), text) : nullptr;
}

/* Copy each node of a list. */
static std::vector<sq_node_t *> copy_items(const std::vector<sq_node_t *> &from) {
  std::vector<sq_node_t *> to;
  to.reserve(from.size());
  for (const sq_node_t *item : from) {
    to.push_back(copy_node(item));
  }
  return to;
}

/* Whether two pieces of text are both absent, or the same. */
static bool equal_text(const char *a, const char *b) {
  return (a == nullptr || b == nullptr) ? a == b : strcmp(a, b) == 0;
}

/* Whether two lists hold equal nodes in the same order. */
static bool equal_items(const std::vector<sq_node_t *> &a,
                        const std::vector<sq_node_t *> &b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); i++) {
    if (!equal_nodes(a[i], b[i])) {
      return false;
    }
  }
  return true;
}
"""


def copy_calls(model, struct, access, out):
    """Write the statements that copy struct's members from f to t."""
    for kind, name, _ in model.all_fields(struct):
        to, frm = f"t{access}{name}", f"f{access}{name}"
        if kind == "embedded":
            ctype = next(f.ctype for k, f in model.fields(struct) if snake(f.name) == name)
            copy_calls(model, ctype, f"{access}{name}.", out)
        elif kind == "list":
            out.write(f"  {to} = copy_items({frm});\n")
        elif kind == "string":
            out.write(f"  {to} = copy_text({frm});\n")
        elif kind == "node":
            out.write(f"  {to} = copy_node({frm});\n")
        elif kind == "typed":
            out.write(f"  {to} = static_cast<decltype({to})>(copy_node({frm}));\n")
        else:
            out.write(f"  {to} = {frm};\n")


def equal_calls(model, struct, access, out):
    """Write the tests that compare struct's members of a and b."""
    for kind, name, attrs in model.all_fields(struct):
        a, b = f"a{access}{name}", f"b{access}{name}"
        # PostgreSQL's equal() ignores where a node was written.
        if kind == "location" or "equal_ignore" in attrs:
            continue
        if kind == "embedded":
            ctype = next(f.ctype for k, f in model.fields(struct) if snake(f.name) == name)
            equal_calls(model, ctype, f"{access}{name}.", out)
        elif kind == "list":
            out.write(f"  if (!equal_items({a}, {b})) return false;\n")
        elif kind == "string":
            out.write(f"  if (!equal_text({a}, {b})) return false;\n")
        elif kind in ("node", "typed"):
            out.write(f"  if (!equal_nodes({a}, {b})) return false;\n")
        else:
            out.write(f"  if ({a} != {b}) return false;\n")


def write_funcs(model, out):
    """Write build/gen/node_funcs.cc."""
    w = out.write
    w(FUNCS_TOP)
    w("\n/* A deep copy of node, made in the tree being built. */\n")
    w("sq_node_t *copy_node(const sq_node_t *node) {\n")
    w("  if (node == nullptr) {\n    return nullptr;\n  }\n")
    w("  switch (node->tag) {\n")
    w("  case SQ_T_LIST: {\n    sq_list_t *t = make_node<sq_list_t>();\n")
    w("    t->items = copy_items(static_cast<const sq_list_t *>(node)->items);\n")
    w("    return t;\n  }\n")
    for s in model.structs:
        w(f"  case {tag_name(s)}: {{\n")
        w(f"    const {type_name(s)} &f = *static_cast<const {type_name(s)} *>(node);\n")
        w(f"    {type_name(s)} &t = *make_node<{type_name(s)}>();\n")
        w("    (void)f;\n")
        copy_calls(model, s, ".", out)
        w("    return &t;\n  }\n")
    w("  default:\n    return nullptr;\n  }\n}\n")

    w("\n/* Whether two trees are the same but for where they were written. */\n")
    w("bool equal_nodes(const sq_node_t *na, const sq_node_t *nb) {\n")
    w("  if (na == nb) {\n    return true;\n  }\n")
    w("  if (na == nullptr || nb == nullptr || na->tag != nb->tag) {\n")
    w("    return false;\n  }\n")
    w("  switch (na->tag) {\n")
    w("  case SQ_T_LIST:\n")
    w("    return equal_items(static_cast<const sq_list_t *>(na)->items,\n")
    w("                       static_cast<const sq_list_t *>(nb)->items);\n")
    for s in model.structs:
        w(f"  case {tag_name(s)}: {{\n")
        w(f"    const {type_name(s)} &a = *static_cast<const {type_name(s)} *>(na);\n")
        w(f"    const {type_name(s)} &b = *static_cast<const {type_name(s)} *>(nb);\n")
        w("    (void)a;\n    (void)b;\n")
        equal_calls(model, s, ".", out)
        w("    return true;\n  }\n")
    w("  default:\n    return false;\n  }\n}\n\n} // namespace sq\n")


CONVERTER_TOP = """\
/*
 * sq_convert.c -- sq_convert: a tree of sq's nodes as PostgreSQL's.
 *
 * GENERATED by ../sq/gen/gen_nodes.py from PostgreSQL's node headers
 * in ../sq/spec/. Do not edit: change the generator.
 *
 * sq parses into its own nodes (../sq/include/sq_nodes.h), and the
 * analyzer reads PostgreSQL's (nodes/parsenodes.h). Each of sq's is
 * PostgreSQL's of the same name with its fields renamed, so this is
 * one function a node, copying field by field: a list into a List,
 * text into a palloc'd string, a node by converting it. What it makes
 * is in the current memory context.
 */

/* sq's header first: it includes the C++ library, which must be read
 * before postgres.h defines macros over names of its own. */
#include "sq.h"

#include "postgres.h"

#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "nodes/value.h"
#include "parser/sq_convert.h"

/* Convert each node of one of sq's lists; NIL for an empty one. */
static List *convert_items(const std::vector<sq_node_t *> &items) {
  List *list = NIL;
  for (const sq_node_t *item : items)
    list = lappend(list, sq_convert(item));
  return list;
}

/* Copy text into the current memory context; NULL stays NULL. */
static char *convert_text(const char *text) {
  return text ? pstrdup(text) : NULL;
}
"""


def convert_calls(model, struct, to, frm, out):
    """Write the statements that fill PostgreSQL's fields from sq's."""
    for kind, f in model.fields(struct):
        dst, src = f"{to}{f.name}", f"{frm}{snake(f.name)}"
        if kind == "embedded":
            convert_calls(model, f.ctype, dst + ".", src + ".", out)
        elif kind == "list":
            out.write(f"  {dst} = convert_items({src});\n")
        elif kind == "string":
            out.write(f"  {dst} = convert_text({src});\n")
        elif kind in ("node", "typed"):
            out.write(f"  {dst} = (decltype({dst}))sq_convert({src});\n")
        elif kind == "enum" and f.ctype == "ObjectType":
            out.write(f"  {dst} = (decltype({dst}))convert_object_type({src});\n")
        elif kind == "enum":
            # The values are PostgreSQL's (sq_nodes.h).
            out.write(f"  {dst} = (decltype({dst}))(int){src};\n")
        else:
            out.write(f"  {dst} = {src};\n")


def write_converter(model, out):
    """Write digres's src/backend/parser/sq_convert.c."""
    w = out.write
    w(CONVERTER_TOP)
    w("\n/* An ObjectType, refusing a kind of object digres has none of. */\n")
    w("static int convert_object_type(int objtype) {\n")
    for what in sorted(set(REFUSED_OBJECTS.values())):
        names = [n for n, v in REFUSED_OBJECTS.items() if v == what]
        w("  if (" + " ||\n      ".join(f"objtype == SQ_{n}" for n in names) + ")\n")
        w("    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),\n")
        w(f'                    errmsg("{what} are not supported")));\n')
    w("  return objtype;\n}\n")
    for s in model.structs:
        if s in REFUSED or s in UNDER_REFUSED:
            continue
        w(f"\n/* Convert a {s}. */\n")
        w(f"static Node *convert_{snake(s)}(const {type_name(s)} *f) {{\n")
        w(f"  {s} *t = makeNode({s});\n\n")
        if s == "A_Const":
            w("  // sq's holds a pointer to a value node; PostgreSQL's, the value.\n")
            w("  if (f->val != NULL) {\n    switch (f->val->tag) {\n")
            for v, member, field, how in (
                ("Integer", "ival", "ival", "{}"),
                ("Float", "fval", "fval", "convert_text({})"),
                ("Boolean", "boolval", "boolval", "{}"),
                ("String", "sval", "sval", "convert_text({})"),
                ("BitString", "bsval", "bsval", "convert_text({})"),
            ):
                src = f"((const {type_name(v)} *)f->val)->{field}"
                w(f"    case {tag_name(v)}:\n")
                w(f"      t->val.{member}.type = T_{v};\n")
                w(f"      t->val.{member}.{field} = {how.format(src)};\n")
                w("      break;\n")
            w("    default:\n")
            w('      elog(ERROR, "unexpected value in an A_Const: %d", (int)f->val->tag);\n')
            w("    }\n  }\n")
        if not model.fields(s):
            w("  (void)f;\n")
        convert_calls(model, s, "t->", "f->", out)
        w("  return (Node *)t;\n}\n")

    w("\n/* PostgreSQL's node for one of sq's, and for all under it. */\n")
    w("Node *sq_convert(const sq_node_t *node) {\n")
    w("  if (node == NULL)\n    return NULL;\n\n")
    w("  // A tree is as deep as the SQL is nested.\n")
    w("  check_stack_depth();\n\n")
    w("  switch (node->tag) {\n")
    w("  case SQ_T_LIST:\n")
    w("    return (Node *)convert_items(((const sq_list_t *)node)->items);\n")
    for s in model.structs:
        if s in UNDER_REFUSED:
            continue
        w(f"  case {tag_name(s)}:\n")
        if s in REFUSED:
            w("    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),\n")
            w(f'                    errmsg("{REFUSED[s]} are not supported")));\n')
            w("    return NULL;\n")
            continue
        w(f"    return convert_{snake(s)}((const {type_name(s)} *)node);\n")
    w("  default:\n")
    w('    elog(ERROR, "unexpected node from the parser: %d", (int)node->tag);\n')
    w("    return NULL;\n  }\n}\n")


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
    writers = {
        "header": write_header,
        "printer": write_printer,
        "funcs": write_funcs,
        "converter": write_converter,
        "fields": write_fields,
    }
    if len(sys.argv) != 2 or sys.argv[1] not in writers:
        sys.exit("usage: gen_nodes.py header|printer|funcs|converter|fields")
    writers[sys.argv[1]](model_t(), sys.stdout)


if __name__ == "__main__":
    main()
