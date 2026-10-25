#!/usr/bin/env python3
"""gen_cat.py -- write digres's built-in catalog rows as C tables.

    gen_cat.py POSTGRES_BKI header > src/include/catalog/cat_builtin.h
    gen_cat.py POSTGRES_BKI source > src/backend/catalog/cat_builtin.c

digres has no catalog tables for what is built in: types, functions,
operators and the rest are compiled in and never change
(sm/PART4.md section 10). This writes them, from the rows genbki.pl
has already resolved into postgres.bki: every reference there is an
oid, and every default is filled in.

For each catalog named in CATALOGS it writes

  - a struct, cat_NAME_t, with a field for each column, by the
    column's own name;
  - the rows, in order of their key, and cat_NAME(key) to find one: the
    key is the oid, or for a catalog with none the column CATALOGS
    names, as pg_aggregate's aggfnoid;
  - where the catalog has a name column worth searching by, the rows
    again in name order, and cat_NAMEs_named(name, &count) to find
    all of that name;
  - cat_NAMEs(&count) (or cat_NAMEes for a name ending in s), all the
    rows, for a caller to search or list.

A row is read-only and lives for ever: a caller keeps the pointer for
as long as it likes, and there is nothing to release.

A catalog is added to CATALOGS when something first asks for it.

For each catalog named in RELATIONS it writes its oid, row type and
columns, and cat_relation(oid) and cat_relation_named(name) to find
one: a catalog relation, a catalog a query can read (SYSCATALOG.md).
For one in TEXT_ROWS it writes its rows too, each value as text, NULL
for a null.
"""

import re
import sys

# catalog -> (name of its rows in C, the column to search by name, or
# None for a catalog that has no name[, the key column, if not the oid])
CATALOGS = {
    "pg_namespace": ("namespace", "nspname"),
    "pg_type": ("type", "typname"),
    "pg_proc": ("proc", "proname"),
    "pg_operator": ("operator", "oprname"),
    "pg_cast": ("cast", None),
    "pg_am": ("am", "amname"),
    "pg_amop": ("amop", None),
    "pg_opclass": ("opclass", None),
    "pg_amproc": ("amproc", None),
    "pg_opfamily": ("opfamily", "opfname"),
    "pg_aggregate": ("aggregate", None, "aggfnoid"),
    "pg_range": ("range", None, "rngtypid"),
    "pg_collation": ("collation", "collname"),
}

#The catalogs a query can read as relations(SYSCATALOG.md).
RELATIONS = ["pg_am", "pg_attrdef", "pg_attribute", "pg_cast", "pg_class",
             "pg_collation", "pg_constraint", "pg_database", "pg_depend",
             "pg_description", "pg_extension", "pg_index", "pg_inherits",
             "pg_language", "pg_namespace", "pg_operator", "pg_policy", "pg_proc",
             "pg_publication", "pg_publication_namespace",
             "pg_publication_rel", "pg_rewrite", "pg_sequence",
             "pg_statistic_ext", "pg_trigger", "pg_type"]

# Views PostgreSQL makes at initdb (system_views.sql), read as catalog
# relations too: name -> (oid, columns). Not in postgres.bki, so their
# columns are written here, as the view's select list has them, and each
# takes an oid from the range initdb gives its views, 12000 to 16383.
VIEWS = {
    "pg_roles": (12100, [
        ("rolname", "name"), ("rolsuper", "bool"), ("rolinherit", "bool"),
        ("rolcreaterole", "bool"), ("rolcreatedb", "bool"),
        ("rolcanlogin", "bool"), ("rolreplication", "bool"),
        ("rolconnlimit", "int4"), ("rolpassword", "text"),
        ("rolvaliduntil", "timestamptz"), ("rolbypassrls", "bool"),
        ("rolconfig", "_text"), ("oid", "oid")]),
    "pg_views": (12101, [
        ("schemaname", "name"), ("viewname", "name"), ("viewowner", "name"),
        ("definition", "text")]),
}

# Of those, the ones whose rows are postgres.bki's, written out as the text
# of each value, for the catalog's own input functions to read.
TEXT_ROWS = {"pg_cast", "pg_collation", "pg_language", "pg_operator",
             "pg_proc", "pg_type"}


# catalog -> {name: its key columns}: the rows in order of those columns,
# and cat_SHORTs_by_NAME() to find the ones with given values, a binary
# search where a scan of every row would be: PostgreSQL's syscache
# indexes on them.
INDEXES = {
    "pg_amop": {
        "operator": ["amopopr"],
        "strategy": ["amopfamily", "amoplefttype", "amoprighttype",
                     "amopstrategy"],
    },
}


def key_col(entry):
    """The column a catalog's rows are ordered and found by."""
    return entry[2] if len(entry) > 2 else "oid"

#BKI column type->(C type, how a value is written)
SCALARS = {
    "oid": ("Oid", "num"),
    "regproc": ("Oid", "regproc"),
    "xid": ("uint32", "num"),
    "int2": ("int16", "num"),
    "int4": ("int32", "num"),
    "int8": ("int64", "num"),
    "float4": ("float", "num"),
    "bool": ("bool", "bool"),
    "char": ("char", "char"),
    "name": ("const char *", "str"),
    "text": ("const char *", "str"),
    "pg_node_tree": ("const char *", "str"),
}
OID_LISTS = {"oidvector", "_oid"}

#Values postgres.bki leaves for the bootstrap to fill in.digres runs
#on 64 - bit Linux alone(sm / PART4.md section 10), so each is known here.
SYMBOLS = {
    "SIZEOF_POINTER": "8",
    "ALIGNOF_POINTER": "d",
    "NAMEDATALEN": "64",
    "FLOAT8PASSBYVAL": "t",
}


def tokens(line):
    """Split the inside of an insert's parentheses into its values."""
    out, i, n = [], 0, len(line)
    while i < n:
        if line[i].isspace():
            i += 1
        elif line[i] == "'":
            j, val = i + 1, ""
            while True:
                if line[j] == "'" and line[j + 1 : j + 2] == "'":
                    val += "'"
                    j += 2
                elif line[j] == "'":
                    break
                else:
                    val += line[j]
                    j += 1
            out.append(("q", val))
            i = j + 1
        else:
            j = i
            while j < n and not line[j].isspace():
                j += 1
            out.append(("w", SYMBOLS.get(line[i:j], line[i:j])))
            i = j
    return out


def read_bki(path):
    """The catalogs of postgres.bki: name -> (columns, rows); each
    catalog's oid: name -> oid; and what its columns force of NOT NULL:
    name -> [True for FORCE NOT NULL, False for FORCE NULL, else None]."""
    text = open(path).read()
    catalogs, oids, forced = {}, {}, {}
    for m in re.finditer(r"^create (\w+) (\d+).*?\n \(\n(.*?)\n \)\n", text, flags=re.M | re.S):
        cols, force = [], []
        for line in m.group(3).split("\n"):
            name, ctype = [p.strip() for p in line.strip(" ,").split("=")]
            cols.append((name, ctype.split()[0]))
            force.append(True if ctype.endswith("FORCE NOT NULL")
                         else False if ctype.endswith("FORCE NULL") else None)
        catalogs[m.group(1)] = (cols, [])
        oids[m.group(1)] = int(m.group(2))
        forced[m.group(1)] = force
    current = None
    for line in text.split("\n"):
# A bootstrap catalog is open from its "create"; the rest are
        # opened by name.
        if line.startswith(("open ", "create ")):
            current = line.split()[1]
        elif line.startswith("close "):
            current = None
        elif line.startswith("insert ( ") and current in catalogs:
            catalogs[current][1].append(tokens(line[len("insert ( ") : -2]))
    return catalogs, oids, forced


def not_nulls(cols, force, typlen):
    """Whether each column is NOT NULL, as PostgreSQL's bootstrap decides:
    as forced, else if it is fixed-width and so is every column before it,
    each NOT NULL."""
    out = []
    for i, (_, ctype) in enumerate(cols):
        if force[i] is not None:
            out.append(force[i])
        else:
            out.append(typlen[ctype] > 0 and
                       all(typlen[c] > 0 and out[j] for j, (_, c) in enumerate(cols[:i])))
    return out


def c_string(value):
    """A C string literal for value."""
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def is_null(tok):
    """Whether a value is BKI's null."""
    return tok == ("w", "_null_")


def field_type(ctype):
    """The C type of a column."""
    if ctype in SCALARS:
        return SCALARS[ctype][0]
    if ctype in OID_LISTS:
        return "cat_oids_t"
    # Arrays of anything else are kept as PostgreSQL writes them, until
    # something needs one taken apart.
    return "const char *"


def field_value(ctype, tok, row_name, col):
    """The initializer for one value, and any array it needs before it."""
    if ctype in OID_LISTS:
        if is_null(tok) or not tok[1].strip("{} "):
            return "{0, NULL}", None
        oids = re.split(r"[ ,]+", tok[1].strip("{} "))
        array = f"static const Oid {row_name}_{col}[] = {{{', '.join(oids)}}};\n"
        return f"{{{len(oids)}, {row_name}_{col}}}", array
    how = SCALARS[ctype][1] if ctype in SCALARS else "str"
    if is_null(tok):
        return ("NULL" if how == "str" else "0"), None
    if how == "num":
        return tok[1], None
    if how == "regproc":
        return ("0" if tok[1] == "-" else tok[1]), None
    if how == "bool":
        return ("true" if tok[1] == "t" else "false"), None
    if how == "char":
        return "'" + tok[1].replace("\\", "\\\\").replace("'", "\\'") + "'", None
    return c_string(tok[1]), None


def write_header(catalogs, out):
    """Write src/include/catalog/cat_builtin.h."""
    w = out.write
    w("/*\n * cat_builtin.h\n *\t  The built-in rows of the catalog, compiled in.\n *\n")
    w(" * GENERATED by src/backend/catalog/gen_cat.py from postgres.bki.\n")
    w(" * Do not edit: change the generator.\n *\n")
    w(" * A row is read-only and lives for ever. A lookup returns a pointer to\n")
    w(" * it, or NULL, and there is nothing to release.\n */\n")
    w("#ifndef CAT_BUILTIN_H\n#define CAT_BUILTIN_H\n\n")
    w("/* A list of oids in a row: an oidvector, or an oid[]. */\n")
    w("typedef struct cat_oids_t {\n  int count;\n  const Oid *values;\n} cat_oids_t;\n")
    for catalog, entry in CATALOGS.items():
        short, name_col, key = entry[0], entry[1], key_col(entry)
        cols, rows = catalogs[catalog]
        w(f"\n/* A row of {catalog}. */\ntypedef struct cat_{short}_t {{\n")
        for col, ctype in cols:
            t = field_type(ctype)
            w(f"  {t}{'' if t.endswith('*') else ' '}{col};\n")
        w(f"}} cat_{short}_t;\n\n")
        w(f"/* The {catalog} row with this {key}, or NULL. */\n")
        w(f"extern const cat_{short}_t *cat_{short}(Oid {key});\n\n")
        w(f"/* Every {catalog} row, in {key} order, and how many in *count. */\n")
        w(f"extern const cat_{short}_t *cat_{plural(short)}(int *count);\n")
        for index, keys in INDEXES.get(catalog, {}).items():
            types = dict(cols)
            params = ", ".join(f"{field_type(types[k])} {k}" for k in keys)
            w(f"\n/*\n * The {catalog} rows with these {', '.join(keys)}, in oid order,\n")
            w(" * and how many in *count; NULL and 0 if there are none.\n */\n")
            w(f"extern const cat_{short}_t *const *cat_{plural(short)}_by_{index}({params},\n")
            w(f"{' ' * 4}int *count);\n")
        if name_col is None:
            continue
        w("\n")
        w(f"/*\n * The {catalog} rows whose {name_col} is name, in oid order, and how\n")
        w(" * many in *count; NULL and 0 if there are none.\n */\n")
        w(f"extern const cat_{short}_t *const *cat_{short}s_named(const char *name,\n")
        w(f"{' ' * (len(short) * 2 + 43)}int *count);\n")
    w("\n/* A column of a catalog relation: its name, its type, whether NOT NULL. */\n")
    w("typedef struct cat_column_t {\n  const char *name;\n  Oid typid;\n  bool notnull;\n} cat_column_t;\n\n")
    w("/* A catalog relation: a catalog a query can read (SYSCATALOG.md). */\n")
    w("typedef struct cat_relation_t {\n  const char *name;\n  Oid oid;\n  Oid rowtype;\n")
    w("  int natts;\n  const cat_column_t *columns;\n")
    w("  /* Its rows, natts values a row, as text, NULL for null; or none. */\n")
    w("  int nrows;\n  const char *const *text;\n} cat_relation_t;\n\n")
    w("/* The catalog relation with this oid, or NULL. */\n")
    w("extern const cat_relation_t *cat_relation(Oid oid);\n\n")
    w("/* The catalog relation named name, or NULL. */\n")
    w("extern const cat_relation_t *cat_relation_named(const char *name);\n\n")
    w("/* Every catalog relation, and how many in *count. */\n")
    w("extern const cat_relation_t *cat_relations(int *count);\n")
    w("\n#endif /* CAT_BUILTIN_H */\n")


def plural(short):
    """The plural of a row's name in C: cast -> casts, opclass -> opclasses."""
    return short + ("es" if short.endswith("s") else "s")


def write_oid_lookup(catalog, short, key, w):
    """Write cat_SHORT(key), a binary search of the rows in key order."""
    w(f"/* Find the {catalog} row with this {key}: a binary search. */\n")
    w(f"const cat_{short}_t *cat_{short}(Oid {key}) {{\n")
    w(f"  int lo = 0, hi = lengthof({short}_rows) - 1;\n\n")
    w("  while (lo <= hi) {\n    int mid = (lo + hi) / 2;\n\n")
    w(f"    if ({short}_rows[mid].{key} == {key})\n      return &{short}_rows[mid];\n")
    w(f"    if ({short}_rows[mid].{key} < {key})\n      lo = mid + 1;\n    else\n      hi = mid - 1;\n")
    w("  }\n  return NULL;\n}\n\n")


def write_source(catalogs, out):
    """Write src/backend/catalog/cat_builtin.c."""
    w = out.write
    w("/*\n * cat_builtin.c\n *\t  The built-in rows of the catalog, and their lookups.\n *\n")
    w(" * GENERATED by src/backend/catalog/gen_cat.py from postgres.bki.\n")
    w(" * Do not edit: change the generator.\n */\n")
    w('#include "postgres.h"\n\n#include "catalog/cat_builtin.h"\n')
    for catalog, entry in CATALOGS.items():
        short, name_col, key = entry[0], entry[1], key_col(entry)
        cols, rows = catalogs[catalog]
        names = [c for c, _ in cols]
        oid_at = names.index(key)
        rows = sorted(rows, key=lambda r: int(r[oid_at][1]))
        body = []
        w(f"\n/* ---- {catalog} ---- */\n\n")
        for row in rows:
            row_name = f"{short}_{row[oid_at][1]}"
            values = []
            for (col, ctype), tok in zip(cols, row):
                value, array = field_value(ctype, tok, row_name, col)
                if array:
                    w(array)
                values.append(value)
            body.append("    {" + ", ".join(values) + "},\n")
        w(f"\nstatic const cat_{short}_t {short}_rows[] = {{\n")
        w("".join(body))
        w("};\n\n")
        write_oid_lookup(catalog, short, key, w)
        w(f"/* Every {catalog} row, in {key} order. */\n")
        w(f"const cat_{short}_t *cat_{plural(short)}(int *count) {{\n")
        w(f"  *count = lengthof({short}_rows);\n  return {short}_rows;\n}}\n")
        for index, keys in INDEXES.get(catalog, {}).items():
            write_index(short, names, cols, rows, index, keys, w)
        if name_col is None:
            continue
        w("\n")
        name_at = names.index(name_col)
        order = sorted(range(len(rows)), key=lambda i: (rows[i][name_at][1].encode(), i))
        w(f"/* The rows in order of {name_col}, and of oid within a name. */\n")
        w(f"static const cat_{short}_t *const {short}_by_name[] = {{\n")
        for i in order:
            w(f"    &{short}_rows[{i}],\n")
        w("};\n\n")
        w(f"/* Find the {catalog} rows named name: a binary search for the first. */\n")
        w(f"const cat_{short}_t *const *cat_{short}s_named(const char *name, int *count) {{\n")
        w(f"  int lo = 0, hi = lengthof({short}_by_name);\n  int n = 0;\n\n")
        w("  /* The first row whose name is not less than the one wanted. */\n")
        w("  while (lo < hi) {\n    int mid = (lo + hi) / 2;\n\n")
        w(f"    if (strcmp({short}_by_name[mid]->{name_col}, name) < 0)\n      lo = mid + 1;\n")
        w("    else\n      hi = mid;\n  }\n")
        w(f"  while (lo + n < (int)lengthof({short}_by_name) &&\n")
        w(f"         strcmp({short}_by_name[lo + n]->{name_col}, name) == 0)\n    n++;\n")
        w("  *count = n;\n")
        w(f"  return n > 0 ? &{short}_by_name[lo] : NULL;\n}}\n")


def write_index(short, names, cols, rows, index, keys, w):
    """Write the rows in order of keys, and cat_SHORTs_by_INDEX()."""
    at = [names.index(k) for k in keys]
    oid_at = names.index("oid")
    order = sorted(range(len(rows)),
                   key=lambda i: tuple(int(rows[i][a][1]) for a in at + [oid_at]))
    arr = f"{short}_by_{index}"
    types = dict(cols)
    params = ", ".join(f"{field_type(types[k])} {k}" for k in keys)
    w(f"\n/* The rows in order of {', '.join(keys)}, and of oid within them. */\n")
    w(f"static const cat_{short}_t *const {arr}[] = {{\n")
    for i in order:
        w(f"    &{short}_rows[{i}],\n")
    w("};\n\n")
    w(f"/* Whether row r comes before the rows with these {', '.join(keys)}. */\n")
    w(f"static bool {arr}_before(const cat_{short}_t *r, {params}) {{\n")
    for k in keys[:-1]:
        w(f"  if (r->{k} != {k})\n    return r->{k} < {k};\n")
    w(f"  return r->{keys[-1]} < {keys[-1]};\n}}\n\n")
    w(f"/* Find the rows with these {', '.join(keys)}: a binary search for the first. */\n")
    w(f"const cat_{short}_t *const *cat_{plural(short)}_by_{index}({params}, int *count) {{\n")
    w(f"  int lo = 0, hi = lengthof({arr});\n  int n = 0;\n\n")
    w("  while (lo < hi) {\n    int mid = (lo + hi) / 2;\n\n")
    w(f"    if ({arr}_before({arr}[mid], {', '.join(keys)}))\n      lo = mid + 1;\n")
    w("    else\n      hi = mid;\n  }\n")
    match = " && ".join(f"{arr}[lo + n]->{k} == {k}" for k in keys)
    w(f"  while (lo + n < (int)lengthof({arr}) && {match})\n    n++;\n")
    w("  *count = n;\n")
    w(f"  return n > 0 ? &{arr}[lo] : NULL;\n}}\n")


def write_relations(catalogs, oids, forced, w):
    """Write the catalog relations of RELATIONS, and their lookups."""
    tcols, trows = catalogs["pg_type"]
    tnames = [c for c, _ in tcols]
    at = {c: tnames.index(c) for c in ("oid", "typname", "typrelid", "typlen")}
    type_oid = {r[at["typname"]][1]: r[at["oid"]][1] for r in trows}
    typlen = {r[at["typname"]][1]: int(r[at["typlen"]][1]) for r in trows}
    rowtype = {r[at["typrelid"]][1]: r[at["oid"]][1] for r in trows}
    w("\n/* ---- catalog relations ---- */\n\n")
    for view, (oid, cols) in VIEWS.items():
        catalogs[view] = (cols, [])
        oids[view] = oid
    for rel in RELATIONS + list(VIEWS):
        w(f"static const cat_column_t {rel}_columns[] = {{\n")
        cols = catalogs[rel][0]
        # A view's columns are its select list's: none is NOT NULL.
        notnull = (not_nulls(cols, forced[rel], typlen) if rel in forced
                   else [False] * len(cols))
        for (col, ctype), nn in zip(cols, notnull):
            w(f"    {{{c_string(col)}, {type_oid[ctype]}, {'true' if nn else 'false'}}},\n")
        w("};\n\n")
        if rel in TEXT_ROWS:
            w(f"static const char *const {rel}_text[] = {{\n")
            for row in catalogs[rel][1]:
                vals = ["NULL" if is_null(t) else c_string(t[1]) for t in row]
                w("    " + ", ".join(vals) + ",\n")
            w("};\n\n")
    w("static const cat_relation_t relations[] = {\n")
    for rel in RELATIONS + list(VIEWS):
        oid = oids[rel]
        w(f"    {{{c_string(rel)}, {oid}, {rowtype.get(str(oid), '0')}, ")
        w(f"lengthof({rel}_columns), {rel}_columns, ")
        if rel in TEXT_ROWS:
            w(f"{len(catalogs[rel][1])}, {rel}_text}},\n")
        else:
            w("0, NULL},\n")
    w("};\n\n")
    w("/* Find the catalog relation with this oid: there are few. */\n")
    w("const cat_relation_t *cat_relation(Oid oid) {\n")
    w("  for (const cat_relation_t &r : relations)\n")
    w("    if (r.oid == oid)\n      return &r;\n  return NULL;\n}\n\n")
    # By name, for a binary search: every unqualified name a query
    # reads is looked for here first, and most are not here.
    rels = RELATIONS + list(VIEWS)
    w("static const cat_relation_t *const relations_by_name[] = {\n")
    for rel in sorted(rels):
        w(f"    &relations[{rels.index(rel)}],\n")
    w("};\n\n")
    w("/* Find the catalog relation named name: a binary search. */\n")
    w("const cat_relation_t *cat_relation_named(const char *name) {\n")
    w("  int lo = 0, hi = lengthof(relations_by_name) - 1;\n\n")
    w("  while (lo <= hi) {\n    int mid = (lo + hi) / 2;\n")
    w("    int c = strcmp(relations_by_name[mid]->name, name);\n\n")
    w("    if (0 == c)\n      return relations_by_name[mid];\n")
    w("    if (c < 0)\n      lo = mid + 1;\n    else\n      hi = mid - 1;\n")
    w("  }\n  return NULL;\n}\n\n")
    w("/* Every catalog relation. */\n")
    w("const cat_relation_t *cat_relations(int *count) {\n")
    w("  *count = lengthof(relations);\n  return relations;\n}\n")


def main():
    """Write the file named on the command line to standard output."""
    writers = {"header": write_header, "source": write_source}
    if len(sys.argv) != 3 or sys.argv[2] not in writers:
        sys.exit("usage: gen_cat.py POSTGRES_BKI header|source")
    catalogs, oids, forced = read_bki(sys.argv[1])
    writers[sys.argv[2]](catalogs, sys.stdout)
    if sys.argv[2] == "source":
        write_relations(catalogs, oids, forced, sys.stdout.write)


if __name__ == "__main__":
    main()
