#!/usr/bin/env python3
"""
cxx_keywords.py -- rename identifiers that are C++ keywords.

PostgreSQL is C, and names variables new, class, namespace,
typename and so on. digres compiles the same files as C++, where
those are reserved. This renames each such identifier by adding
an underscore: new becomes new_.

It reads C well enough for that and no further: comments, string
and character literals are left as they are, and so are the words of an #error
line and anything inside #ifdef __cplusplus, which is C++ already.

    cxx_keywords.py FILE...     rewrite the files in place
"""
import re
import sys

# The C++20 keywords that C does not have. asm, bool, true, false,
# inline, wchar_t and static_assert are left out: C code that uses
# them means what C++ means.
KEYWORDS = set("""
and and_eq bitand bitor catch class compl concept const_cast consteval
constexpr constinit co_await co_return co_yield decltype delete
dynamic_cast explicit export friend mutable namespace new noexcept not
not_eq nullptr operator or or_eq private protected public
reinterpret_cast requires static_cast template this throw try typeid
typename using virtual xor xor_eq
""".split())

TOKEN = re.compile(
    r"""
      (?P<comment>/\*.*?\*/|//[^\n]*)
    | (?P<error>(?<![^\n])[ \t]*\#[ \t]*error[^\n]*)
    | (?P<string>"(?:\\.|[^"\\\n])*")
    | (?P<char>'(?:\\.|[^'\\\n])*')
    | (?P<ident>[A-Za-z_][A-Za-z0-9_]*)
    | (?P<other>.)
    """,
    re.S | re.X,
)
CPP_IF = re.compile(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|else|elif|endif)\b(.*)$")


def convert(text):
    """Return text with the keyword identifiers renamed, and how many."""
    out = []
    count = 0
    # Nesting of #if blocks; an entry is True inside __cplusplus.
    stack = []
    line_start = True
    pos = 0
    while pos < len(text):
        if line_start:
            end = text.find("\n", pos)
            line = text[pos : end if end >= 0 else len(text)]
            m = CPP_IF.match(line)
            if m:
                word, rest = m.group(1), m.group(2)
                if word in ("if", "ifdef"):
                    stack.append("__cplusplus" in rest and "!" not in rest
                                 and word != "ifndef")
                elif word == "ifndef":
                    stack.append(False)
                elif word in ("else", "elif") and stack:
                    stack[-1] = False
                elif word == "endif" and stack:
                    stack.pop()
        m = TOKEN.match(text, pos)
        tok = m.group(0)
        if m.lastgroup == "ident" and tok in KEYWORDS and not any(stack):
            tok += "_"
            count += 1
        out.append(tok)
        pos = m.end()
        line_start = tok.endswith("\n")
    return "".join(out), count


def main(paths):
    """Rewrite each file in place, and say how much changed."""
    total = 0
    for path in paths:
        with open(path, encoding="utf-8", errors="surrogateescape") as f:
            text = f.read()
        new_text, count = convert(text)
        if count:
            with open(path, "w", encoding="utf-8",
                      errors="surrogateescape") as f:
                f.write(new_text)
            total += count
    print(f"{total} identifiers renamed in {len(paths)} files")


if __name__ == "__main__":
    main(sys.argv[1:])
