# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

"""nodespec.py -- read PostgreSQL's node headers into structs and enums.

The headers in spec/ are PostgreSQL's own, unedited. This reads them
the way PostgreSQL's gen_node_support.pl does: by their layout, not
with a C parser. A struct is `typedef struct NAME { ... } NAME;`, an
enum is `typedef enum NAME { ... } NAME;`, and a field is one
declaration, with an optional pg_node_attr(...) after it.
"""

import re
from dataclasses import dataclass, field


@dataclass
class field_t:
    """One field of a node struct."""

    ctype: str  # the C type as written, e.g. "List *", "char *", "bool"
    name: str
    attrs: list


@dataclass
class struct_t:
    """One node struct, in declaration order."""

    name: str
    fields: list
    attrs: list
    header: str


@dataclass
class enum_t:
    """One enum, with its enumerators as (name, value or None)."""

    name: str
    members: list
    header: str


@dataclass
class spec_t:
    """Everything read from the headers."""

    structs: dict = field(default_factory=dict)
    enums: dict = field(default_factory=dict)
    typedefs: dict = field(default_factory=dict)  # alias -> C type


def strip_comments(text):
    """Remove C comments and preprocessor lines, keeping line breaks out."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r"^\s*#[^\n]*(\\\n[^\n]*)*", " ", text, flags=re.M)
    return text


def parse_attrs(text):
    """Split the inside of pg_node_attr(...) into its attributes."""
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


ATTR_RE = re.compile(r"pg_node_attr\(((?:[^()]|\([^()]*\))*)\)")
FIELD_RE = re.compile(r"^(?P<type>.+?[\s\*])(?P<name>\w+(?:\[\w*\])?)$", re.S)


def parse_fields(body):
    """Read a struct body into its attributes and fields."""
    struct_attrs, fields = [], []
    for decl in body.split(";"):
        decl = " ".join(decl.split())
        if not decl:
            continue
        attrs = []
        # A pg_node_attr that leads a declaration belongs to the struct.
        m = ATTR_RE.match(decl)
        if m:
            struct_attrs += parse_attrs(m.group(1))
            decl = decl[m.end():].strip()
            if not decl:
                continue
        m = ATTR_RE.search(decl)
        if m:
            attrs = parse_attrs(m.group(1))
            decl = (decl[: m.start()] + decl[m.end():]).strip()
        m = FIELD_RE.match(decl)
        if not m:
            raise ValueError(f"cannot read field: {decl!r}")
        ctype = re.sub(r"\s*\*\s*", " *", m.group("type").strip())
        fields.append(field_t(ctype, m.group("name"), attrs))
    return struct_attrs, fields


def parse_enum(body):
    """Read an enum body into (name, value) pairs."""
    members = []
    for item in body.split(","):
        item = " ".join(item.split())
        if not item:
            continue
        name, _, value = item.partition("=")
        members.append((name.strip(), value.strip() or None))
    return members


def read_header(path, spec):
    """Add one header's structs, enums and typedefs to spec."""
    name = path.split("/")[-1]
    text = strip_comments(open(path).read())
    # A struct is usually typedef'd where it is defined; a few are
    # declared ahead and defined as a plain `struct NAME { ... };`.
    for m in re.finditer(
        r"(?:typedef\s+)?struct\s+(\w+)\s*\{(.*?)\}\s*\w*\s*;", text, flags=re.S
    ):
        attrs, fields = parse_fields(m.group(2))
        spec.structs[m.group(1)] = struct_t(m.group(1), fields, attrs, name)
    for m in re.finditer(
        r"typedef\s+enum\s+(\w+)\s*\{(.*?)\}\s*(\w+)\s*;", text, flags=re.S
    ):
        spec.enums[m.group(3)] = enum_t(m.group(3), parse_enum(m.group(2)), name)
    for m in re.finditer(r"^\s*typedef\s+([\w ]+?)\s+(\w+)\s*;", text, flags=re.M):
        if not m.group(1).startswith(("struct", "enum", "union")):
            spec.typedefs[m.group(2)] = m.group(1)


def read_spec(directory, headers):
    """Read the named headers of directory, in order."""
    spec = spec_t()
    for h in headers:
        read_header(f"{directory}/{h}", spec)
    return spec
