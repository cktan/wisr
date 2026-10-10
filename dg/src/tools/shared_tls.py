#!/usr/bin/env python3
"""
shared_tls.py -- the table of SHARED variables a new thread is given.

Every writable global is thread_local (c.h), and a new thread starts
with each at the value the source wrote. A variable marked /*SHARED*/
is one every session reads as its parent left it, so a session thread
is given a copy of its parent's (utils/init/session_thread.c). This
writes the table that copy goes by: each SHARED variable's offset in
the thread-local block, and its size.

    shared_tls.py SRC SUFFIX MAP PROGRAM
        print the table, as C, for PROGRAM, linked from the objects
        under SRC named *SUFFIX (".o", or ".tsan.o") with the map MAP
    shared_tls.py --empty
        print an empty table, for the first link

The definitions come from the source, where the mark is; the offsets
from each object's symbols, placed by the linker map. A file's
__tls_guard is left out, so a thread runs the file's dynamic
initialisers again (a list head that points at itself, say), and they
overwrite what was copied of those variables. A variable's
offset is where its object's .tdata or .tbss went, less the start of
the program's TLS segment, plus its offset in that section. A
function's static is matched by its name alone, so a file where a
SHARED and a LOCAL one share a name is an error. The table is built
from one link and compiled into the next; the TLS layout does not
change between them, as the table holds no thread-local variable, and
the Makefile checks that a second run prints the same table.
"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cxx_thread_local import mask, names_in, statement_start  # noqa: E402

MARK = re.compile(r"\bthread_local\s*/\*(SHARED|LOCAL)\*/")
# The variable whose address locates the thread-local block at run time.
ANCHOR = "progname"


def run(args):
    """Standard output of ARGS."""
    return subprocess.run(args, check=True, capture_output=True,
                          text=True).stdout


def definitions(path):
    """{name: mark} of the thread_local definitions in the file PATH and
    the .c files it includes: not extern declarations. A name defined
    with both marks maps to None."""
    found = {}
    with open(path) as f:
        text = f.read()
    for inc in re.findall(r'^#include "([^"]+\.c)"', text, re.M):
        inc = os.path.join(os.path.dirname(path), inc)
        if os.path.exists(inc):
            for name, mark in definitions(inc).items():
                found[name] = None if found.get(name, mark) != mark else mark
    # An attribute macro between a name and its initialiser hides it.
    code = re.sub(r"\bPG_USED_FOR_ASSERTS_ONLY\b|\bpg_attribute_\w+\b",
                  lambda m: " " * len(m.group()), mask(text))
    for m in MARK.finditer(text):
        start = statement_start(code, m.start())
        if code.startswith("extern", start):
            continue
        end, depth = m.end(), 0
        while end < len(code) and not (code[end] == ";" and depth == 0):
            depth += {"(": 1, "[": 1, "{": 1, ")": -1, "]": -1,
                      "}": -1}.get(code[end], 0)
            end += 1
        for name in names_in(code, m.end(), end):
            if found.get(name, m.group(1)) != m.group(1):
                found[name] = None
            else:
                found[name] = m.group(1)
    return found


def sections(obj):
    """{index: name} of the sections of the object OBJ."""
    out = {}
    for line in run(["readelf", "-SW", obj]).splitlines():
        m = re.match(r"^\s*\[\s*(\d+)\]\s+(\S+)", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def tls_symbols(obj):
    """[(section, offset, size, demangled name)] of the thread-local
    variables OBJ defines."""
    syms = []
    for line in run(["readelf", "-sW", obj]).splitlines():
        f = line.split()
        if len(f) == 8 and f[3] == "TLS" and f[6].isdigit() and \
                int(f[2], 0) > 0:
            syms.append((f[6], int(f[1], 16), int(f[2], 0), f[7]))
    if not syms:
        return []
    names = run(["c++filt"] + [s[3] for s in syms]).split("\n")
    secs = sections(obj)
    return [(secs[s[0]], s[1], s[2], names[i]) for i, s in enumerate(syms)]


def placements(map_path):
    """{(object, section): address} of every .tdata and .tbss the map
    MAP_PATH places."""
    with open(map_path) as f:
        text = f.read()
    pat = re.compile(r"^ (\.t(?:data|bss))\s+0x([0-9a-f]+)\s+0x[0-9a-f]+"
                     r"\s+(\S+\.o)$", re.M)
    return {(m.group(3), m.group(1)): int(m.group(2), 16)
            for m in pat.finditer(text)}


def tls_start(program):
    """The virtual address of PROGRAM's TLS segment."""
    for line in run(["readelf", "-lW", program]).splitlines():
        f = line.split()
        if f and f[0] == "TLS":
            return int(f[2], 16)
    sys.exit("shared_tls.py: %s has no TLS segment" % program)


def table(src, suffix, map_path, program):
    """[(offset, size, where)] of every SHARED variable, sorted, and the
    anchor's offset."""
    placed = placements(map_path)
    start = tls_start(program)
    entries, anchor = [], None
    for dirpath, _, files in os.walk(src):
        for fname in sorted(files):
            if not fname.endswith(suffix) or \
                    (suffix == ".o" and fname.endswith(".tsan.o")):
                continue
            obj = os.path.join(dirpath, fname)
            source = obj[:-len(suffix)] + ".c"
            syms = tls_symbols(obj)
            if not syms:
                continue
            marks = definitions(source)
            for sec, off, size, name in syms:
                # The guard of the file's dynamic initialisers is not
                # copied: a child runs them again, for itself.
                if name == "__tls_guard":
                    continue
                short = name.rsplit("::", 1)[-1]
                if short not in marks:
                    sys.exit("shared_tls.py: %s: no marked definition of %s"
                             % (source, name))
                if marks[short] is None:
                    sys.exit("shared_tls.py: %s: %s is both SHARED and "
                             "LOCAL" % (source, short))
                where = placed.get((obj, sec))
                if where is None:
                    sys.exit("shared_tls.py: %s %s is not in the map"
                             % (obj, sec))
                offset = where - start + off
                if name == ANCHOR:
                    anchor = offset
                if marks[short] == "SHARED":
                    entries.append((offset, size, "%s %s" % (source, name)))
    if anchor is None:
        sys.exit("shared_tls.py: no %s to anchor the table" % ANCHOR)
    return sorted(entries), anchor


def emit(entries, anchor):
    """Print the table as C."""
    print("/* Written by src/tools/shared_tls.py: do not edit. */")
    print('#include "postgres.h"\n')
    print('#include "utils/session_thread.h"\n')
    print("const size_t shared_tls_anchor = %d;" % anchor)
    print("const int shared_tls_count = %d;" % len(entries))
    print("const shared_tls_entry_t shared_tls_table[] = {")
    for offset, size, where in entries:
        print("    {%d, %d}, /* %s */" % (offset, size, where))
    if not entries:
        print("    {0, 0},")
    print("};")


if __name__ == "__main__":
    if sys.argv[1:] == ["--empty"]:
        emit([], 0)
    elif len(sys.argv) == 5:
        emit(*table(*sys.argv[1:]))
    else:
        sys.exit("usage: shared_tls.py SRC SUFFIX MAP PROGRAM | --empty")
