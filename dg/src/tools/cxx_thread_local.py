#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

"""
cxx_thread_local.py -- make every writable global thread_local.

digres is to run each session in a thread of one process
(DESIGN.md section 3). Every writable global or static is then
thread_local, one copy a thread, and a comment after the keyword
tells what it is (c.h): /*SHARED*/ for one every session reads as
its parent left it, /*LOCAL*/ for one a session keeps for itself.

The objects are the truth, not a search of the source: a variable
is writable and not thread_local when it lies in .bss, .data,
.data.rel or .data.rel.local. A thread_local one lies in .tbss or .tdata.

    cxx_thread_local.py --apply SRC LIST
        mark each writable variable under SRC thread_local, SHARED
        if LIST names it, LOCAL if not, on its definition and on
        every extern declaration of it
    cxx_thread_local.py --check SRC
        fail if a writable variable is not thread_local, or if an object
        has a static initialiser; the variables process_wide.txt lists,
        beside this script, are one for the process by design

LIST has one variable a line, the object named from SRC and the
variable as objdump -C names it, a function's static as
"func(args)::name", with the reason after a '#':

    backend/storage/ipc/shmem.o ShmemBase  # pointer into shared memory

--apply is run once on built objects, and the tree is built again
after it. The line to write is found from nm -l, which names the
line of each definition. A declaration of two variables marked
differently is not written, and is listed.

The second check is there because this is C++: a table that is not
thread_local, whose initialiser holds the address of a thread_local,
as the GUC tables do, compiles, and is filled once by a static
initialiser, with the address of the first thread's copy. No object
has a static initialiser today, so any is an error.
"""
import os
import re
import subprocess
import sys

WRITABLE = {".bss", ".data", ".data.rel", ".data.rel.local"}
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def objects(src):
    """Every object under SRC, sorted, as paths: not the thread
    sanitizer's, which it gives static initialisers, nor the release
    build's, from the same sources as the objects checked."""
    out = []
    for dirpath, _, files in os.walk(src):
        out += [os.path.join(dirpath, f) for f in files
                if f.endswith(".o")
                and not f.endswith((".tsan.o", ".rel.o"))]
    return sorted(out)


def writable(src, demangle):
    """{(object from SRC, symbol): section} of every writable data
    object, and the set of objects with a static initialiser."""
    found, init = {}, set()
    args = ["objdump", "-t", "-h"] + (["-C"] if demangle else [])
    text = subprocess.run(args + objects(src), check=True,
                          capture_output=True, text=True).stdout
    obj = None
    for line in text.splitlines():
        m = re.match(r"^(\S+\.o):\s+file format", line)
        if m:
            obj = os.path.relpath(m.group(1), src)
            continue
        if ".init_array" in line.split():
            init.add(obj)
        # addr flags section size name: the flags are fixed width.
        m = re.match(r"^[0-9a-f]+ (.{7}) (\S+)\t[0-9a-f]+ (.*)$", line)
        if m and "O" in m.group(1) and m.group(2) in WRITABLE:
            found[(obj, m.group(3))] = m.group(2)
    return found, init


def read_list(path):
    """{(object, symbol): reason} from the list of shared variables."""
    shared = {}
    with open(path) as f:
        for line in f:
            body, _, reason = line.partition("#")
            body = body.strip()
            if not body:
                continue
            obj, _, sym = body.partition(" ")
            shared[(obj, sym.strip())] = reason.strip()
    return shared


def check(src):
    """Report each writable variable not thread_local, but for those
    process_wide.txt lists, and each static initialiser; 1 if there
    is any."""
    found, init = writable(src, True)
    # One for the process by design: process_wide.txt says why.
    process_wide = read_list(os.path.join(os.path.dirname(
        os.path.abspath(__file__)), "process_wide.txt"))
    found = {k: v for k, v in found.items() if k not in process_wide}
    for key in sorted(found):
        print("not thread_local: %s %s" % key)
    for obj in sorted(init):
        print("static initialiser: %s" % obj)
    return 1 if found or init else 0


def mask(text):
    """TEXT with comments, literals and preprocessor lines blanked,
    so offsets still match; a directive leaves a ';' as a boundary."""
    out = list(text)
    i, n, bol = 0, len(text), True
    while i < n:
        c = text[i]
        if bol and c == "#":
            j = i
            while j < n and not (text[j] == "\n" and text[j - 1] != "\\"):
                j += 1
            for k in range(i, j):
                out[k] = " " if text[k] != "\n" else "\n"
            out[i] = ";"
            i = j
            continue
        if c == "\n":
            bol = True
        elif not c.isspace():
            bol = False
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2) + 2
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j += 1
            for k in range(i + 1, j - 1):
                if out[k] != "\n":
                    out[k] = " "
            i = j
            continue
        else:
            i += 1
            continue
        for k in range(i, j):
            if out[k] != "\n":
                out[k] = " "
        i = j
    return "".join(out)


def statement_start(code, pos):
    """Offset of the first token of the declaration holding POS.
    A brace pair before POS belongs to it when it is the body of a
    struct, union or enum."""
    i = pos - 1
    while i >= 0:
        c = code[i]
        if c in ";{":
            break
        if c == "}":
            depth, j = 0, i
            while j >= 0:
                depth += {"}": 1, "{": -1}.get(code[j], 0)
                if depth == 0:
                    break
                j -= 1
            head = code[:j].rstrip()
            if not re.search(r"\b(struct|union|enum)(\s+\w+)?$", head):
                break
            i = j - 1
            continue
        i -= 1
    i += 1
    while code[i].isspace():
        i += 1
    return i


def names_in(code, start, end):
    """The names a declaration from START to END declares, at depth
    zero or as (*name)."""
    names, depth = [], 0
    i = start
    while i < end:
        c = code[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "=" and depth == 0:
            # Skip the initialiser to the next comma at depth zero.
            d = 0
            while i < end and not (code[i] == "," and d == 0):
                d += {"(": 1, "[": 1, "{": 1, ")": -1, "]": -1,
                      "}": -1}.get(code[i], 0)
                i += 1
            continue
        m = IDENT.match(code, i)
        if m and (i == 0 or not code[i - 1].isalnum() and code[i - 1] != "_"):
            after = code[m.end():end].lstrip()
            before = code[start:i].rstrip()
            if depth == 0 and after[:1] in (";", ",", "[", "=", ""):
                names.append(m.group())
            elif depth == 1 and before.endswith("(*") and after[:1] == ")":
                names.append(m.group())
            i = m.end()
            continue
        i += 1
    return names


class source_t:
    """A file being written: its text, its mask, and the inserts."""

    def __init__(self, path):
        with open(path) as f:
            self.text = f.read()
        self.code = mask(self.text)
        self.path = path
        self.inserts = {}

    def line_offset(self, line):
        """Offset of the start of LINE, counted from 1."""
        off = 0
        for _ in range(line - 1):
            off = self.text.index("\n", off) + 1
        return off

    def save(self):
        """Write the inserts, last first so offsets hold."""
        text = self.text
        for off in sorted(self.inserts, reverse=True):
            text = text[:off] + self.inserts[off] + text[off:]
        with open(self.path, "w") as f:
            f.write(text)


def locate(src, objs):
    """{(object, mangled): (file, line)} from nm -l."""
    where = {}
    for obj in objs:
        text = subprocess.run(["nm", "-l", "--defined-only",
                               os.path.join(src, obj)], check=True,
                              capture_output=True, text=True).stdout
        for line in text.splitlines():
            m = re.match(r"^\S+ [BDbd] (\S+)\t(.*):(\d+)$", line)
            if m:
                where[(obj, m.group(1))] = (m.group(2), int(m.group(3)))
    return where


def real_path(src, path):
    """PATH as nm gave it, or the file under SRC it ends with: a
    #line in a generated file names its source from dg/."""
    if os.path.exists(path):
        return path
    k = path.rfind("/src/")
    while k >= 0:
        cand = os.path.join(os.path.dirname(os.path.abspath(src)),
                            path[k + 1:])
        if os.path.exists(cand):
            return cand
        k = path.rfind("/src/", 0, k)
    return None


def apply(src, list_path):
    """Mark every writable variable thread_local, SHARED or LOCAL."""
    shared = read_list(list_path)
    plain, _ = writable(src, False)
    names = demangle([sym for _, sym in plain])
    todo = sorted((obj, sym, names[sym]) for obj, sym in plain)
    problems = ["listed, but no writable variable: %s %s" % key
                for key in sorted(set(shared) -
                                  {(o, n) for o, _, n in todo})]
    where = locate(src, sorted({obj for obj, _, _ in todo}))
    generated = set(subprocess.run(
        ["git", "ls-files", "-o", "-i", "--exclude-standard", src],
        check=True, capture_output=True, text=True).stdout.split())
    files, externs = {}, {}

    def get(path):
        """The source_t of PATH, read once."""
        path = os.path.abspath(path)
        if path not in files:
            files[path] = source_t(path)
        return files[path]

    for obj, sym, pretty_name in todo:
        mark = "thread_local /*SHARED*/ " if (obj, pretty_name) in shared \
            else "thread_local /*LOCAL*/ "
        loc = where.get((obj, sym))
        path = loc and real_path(src, loc[0])
        if not path:
            problems.append("no source line: %s %s" % (obj, pretty_name))
            continue
        if os.path.relpath(path) in generated:
            problems.append("generated: %s %s" % (path, pretty_name))
            continue
        short = pretty_name.rsplit("::", 1)[-1]
        s = get(path)
        off = s.line_offset(loc[1])
        m = re.compile(r"\b%s\b" % re.escape(short)).search(
            s.code, off, s.code.find("\n", off))
        if not m:
            problems.append("not on its line: %s:%d %s"
                            % (path, loc[1], short))
            continue
        start = statement_start(s.code, m.start())
        # After "static", where C++ writes "static thread_local".
        if re.match(r"static\s", s.code[start:]):
            start += len("static ")
        if s.inserts.get(start, mark) != mark:
            problems.append("declared with a variable marked otherwise: "
                            "%s:%d %s" % (path, loc[1], short))
            continue
        s.inserts[start] = mark
        if not sym.startswith("_Z"):
            externs[sym] = mark

    # Each extern declaration of a global gets the mark of its
    # definition, or the link fails.
    if externs:
        pat = re.compile(r"\b(%s)\b" % "|".join(sorted(externs)))
        for dirpath, _, fnames in os.walk(src):
            for fname in sorted(fnames):
                if not fname.endswith((".c", ".h", ".l", ".y")):
                    continue
                path = os.path.join(dirpath, fname)
                s = files.get(os.path.abspath(path)) or source_t(path)
                found = False
                for m in pat.finditer(s.code):
                    start = statement_start(s.code, m.start())
                    if not s.code.startswith("extern", start) or \
                            s.code.startswith('extern "', start):
                        continue
                    end = s.code.find(";", m.start())
                    if m.group(1) not in names_in(s.code, start, end):
                        continue
                    if os.path.relpath(path) in generated:
                        problems.append("extern in generated %s: %s"
                                        % (path, m.group(1)))
                        continue
                    s.inserts[start + len("extern")] = \
                        " " + externs[m.group(1)].strip()
                    found = True
                if found:
                    files[os.path.abspath(path)] = s
    for s in files.values():
        s.save()
    for p in problems:
        print(p)
    print("%d files written, %d variables, %d problems"
          % (len(files), len(todo), len(problems)))
    return 1 if problems else 0


def demangle(syms):
    """{mangled: demangled} through c++filt."""
    out = subprocess.run(["c++filt"], input="\n".join(syms), check=True,
                         capture_output=True, text=True).stdout.split("\n")
    return dict(zip(syms, out))


if __name__ == "__main__":
    if sys.argv[1:2] == ["--apply"] and len(sys.argv) == 4:
        sys.exit(apply(sys.argv[2], sys.argv[3]))
    if sys.argv[1:2] == ["--check"] and len(sys.argv) == 3:
        sys.exit(check(sys.argv[2]))
    sys.exit("usage: cxx_thread_local.py --apply SRC LIST | --check SRC")
