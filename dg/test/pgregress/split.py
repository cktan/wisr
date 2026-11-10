#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# split.py -- split a regression file into the units psql runs: an SQL
# statement up to its semicolon or the \g-style command that sends it, a
# backslash command on a line of its own, and a COPY FROM stdin with its
# data. It follows psql's lexer (src/fe_utils/psqlscan.l) as far as these
# files need: quotes, comments, dollar quotes, parentheses, and the BEGIN
# ATOMIC body of a CREATE FUNCTION or PROCEDURE.
#
#   split.py FILE...   prints each unit's first and last line, to check
import re
import sys

# The backslash commands that send the statement before them.
SENDERS = {'g', 'gx', 'gset', 'gexec', 'gdesc', 'crosstabview', 'watch',
           'bind', 'bind_named', 'parse', 'sendpipeline', 'syncpipeline'}

# The backslash commands whose units must never be taken out of a file:
# the branches of a conditional.
CONDITIONALS = {'if', 'elif', 'else', 'endif'}

DOLLAR = re.compile(r'\$([A-Za-z_\x80-￿][A-Za-z_0-9\x80-￿]*)?\$')
IDENT = re.compile(r'[A-Za-z_\x80-￿][A-Za-z_0-9$\x80-￿]*')


class unit_t:
    """A unit of a file: text[start:end], what kind it is, and for a
    backslash command its name."""

    def __init__(self, start, end, kind, cmd=None):
        self.start = start
        self.end = end
        self.kind = kind  # 'sql', 'meta' or 'copy'
        self.cmd = cmd

    def __repr__(self):
        return 'unit_t(%d, %d, %s, %s)' % (self.start, self.end, self.kind,
                                           self.cmd)


def meta_end(text, i):
    """Where the backslash command at text[i] ends: at the line's end, or
    past a \\\\ that separates it from what follows, outside quotes."""
    n = len(text)
    j = i + 1
    quote = None
    while j < n and text[j] != '\n':
        c = text[j]
        if quote:
            if c == quote:
                quote = None
        elif c in '\'"`':
            quote = c
        elif c == '\\' and j + 1 < n and text[j + 1] == '\\':
            return j + 2
        j += 1
    return j


def line_end(text, i):
    """The index just past the newline of the line text[i] is on."""
    j = text.find('\n', i)
    return len(text) if j < 0 else j + 1


def copy_data_end(text, i):
    """Where a COPY's inline data that starts at text[i] ends: past the
    line that is \\. alone."""
    while i < len(text):
        j = line_end(text, i)
        if text[i:j].rstrip('\r\n') == '\\.':
            return j
        i = j
    return i


def from_stdin(stmt):
    """How many COPYs in a statement read their rows inline: one, or
    more where \\; joins several."""
    n = 0
    for part in stmt.split('\\;'):
        s = re.sub(r'\s+', ' ', part.strip()).lower()
        if re.match(r'^\\?copy\b', s) and re.search(
                r'\bfrom (stdin|stdout|pstdin)\b', s):
            n += 1
    return n


def split(text):
    """The units of text, in order; what is between them is whitespace and
    comments."""
    units = []
    n = len(text)
    i = 0
    start = None  # where the statement being read began
    paren = 0
    begin = 0  # BEGIN ATOMIC depth
    idents = []  # the statement's first identifiers, lower case

    def finish(end, kind='sql', cmd=None):
        nonlocal start, paren, begin, idents
        u = unit_t(start, end, kind, cmd)
        units.append(u)
        start = None
        paren = begin = 0
        idents = []
        # A COPY FROM stdin's rows follow it, after the rest of its line.
        copies = from_stdin(text[u.start:u.end])
        if copies:
            u.kind = 'copy'
            u.end = line_end(text, end - 1)
            for _ in range(copies):
                u.end = copy_data_end(text, u.end)
        return u.end

    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
            continue
        if text.startswith('--', i):
            i = line_end(text, i) if start is None else text.find('\n', i)
            if i < 0:
                i = n
            continue
        if text.startswith('/*', i):
            depth = 0
            while i < n:
                if text.startswith('/*', i):
                    depth += 1
                    i += 2
                elif text.startswith('*/', i):
                    depth -= 1
                    i += 2
                    if depth == 0:
                        break
                else:
                    i += 1
            continue
        if c == '\\':
            if text.startswith('\\;', i):
                start = i if start is None else start
                i += 2
                continue
            m = re.match(r'\\([A-Za-z_]+|[!?.])', text[i:])
            cmd = m.group(1) if m else ''
            end = meta_end(text, i)
            if start is not None:
                if cmd in SENDERS:
                    i = finish(end, 'sql', cmd)
                else:
                    i = end  # a command inside a statement, kept with it
                continue
            start = i
            i = finish(end, 'meta', cmd)
            continue
        if start is None:
            start = i
        if c == "'":
            # A string: E'...' takes backslash escapes, others do not.
            esc = i > 0 and text[i - 1] in 'eE' and (
                i < 2 or not (text[i - 2].isalnum() or text[i - 2] == '_'))
            j = i + 1
            while j < n:
                if esc and text[j] == '\\':
                    j += 2
                    continue
                if text[j] == "'":
                    if j + 1 < n and text[j + 1] == "'":
                        j += 2
                        continue
                    break
                j += 1
            i = j + 1
            continue
        if c == '"':
            j = i + 1
            while j < n:
                if text[j] == '"':
                    if j + 1 < n and text[j + 1] == '"':
                        j += 2
                        continue
                    break
                j += 1
            i = j + 1
            continue
        if c == '$':
            m = DOLLAR.match(text, i)
            if m and not (i > 0 and (text[i - 1].isalnum() or
                                     text[i - 1] == '_')):
                tag = m.group(0)
                j = text.find(tag, m.end())
                i = n if j < 0 else j + len(tag)
                continue
            i += 1
            continue
        if c == ':' and i + 1 < n and text[i + 1] in '\'"':
            # A psql variable, :'name' or :"name".
            j = text.find(text[i + 1], i + 2)
            i = n if j < 0 else j + 1
            continue
        if c == '(':
            paren += 1
            i += 1
            continue
        if c == ')':
            paren = max(paren - 1, 0)
            i += 1
            continue
        if c == ';':
            if paren == 0 and begin == 0:
                i = finish(i + 1)
            else:
                i += 1
            continue
        m = IDENT.match(text, i)
        if m:
            word = m.group(0).lower()
            if len(idents) < 4:
                idents.append(word)
            # psqlscan.l: a CREATE [OR REPLACE] FUNCTION or PROCEDURE's
            # BEGIN ... END body, and the CASEs in it.
            if idents and idents[0] == 'create' and (
                    'function' in idents or 'procedure' in idents):
                if word == 'begin' or (word == 'case' and begin > 0):
                    begin += 1
                elif word == 'end' and begin > 0:
                    begin -= 1
            i = m.end()
            continue
        i += 1
    if start is not None:
        units.append(unit_t(start, n, 'sql'))
    return units


if __name__ == '__main__':
    for path in sys.argv[1:]:
        with open(path, encoding='latin-1') as f:
            text = f.read()
        for u in split(text):
            first = text[u.start:u.end].splitlines()[0]
            print('%s:%d %s %s' % (path, text.count('\n', 0, u.start) + 1,
                                   u.kind, first[:70]))
