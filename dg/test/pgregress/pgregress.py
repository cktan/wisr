#!/usr/bin/env python3
# pgregress.py -- PostgreSQL 18's regression files, ported to digres
# (README.md): each runs on digres as on PostgreSQL 18 once the statements
# digres does not run as PostgreSQL does are commented out, and its
# expected output is PostgreSQL 18's for the file so trimmed.
#
#   pgregress.py [-j N] test [FILE...]      run on digres, compare with
#                                           expected/; exits 1 on a failure
#   pgregress.py [-j N] trim [FILE...]      comment out what digres does
#                                           not run as PostgreSQL does
#   pgregress.py [-j N] expected [FILE...]  write expected/ from PG 18
#   pgregress.py [-j N] check [FILE...]     check split.py against PG 18
#
# FILE is a name in schedule; none means all of them. Each runs alone, in
# a fresh database, after test_setup. trim runs each file on both servers,
# a pass at a time, until they agree; a statement taken out has each line
# prefixed "--dg " under a line that says why:
#
#   --dg-skip: CAUSE      digres refuses it: CAUSE is the error, with
#                         names, numbers and quoted strings taken out
#   --dg-cascade          it does on PostgreSQL what it did not once a
#                         statement before it was taken out
#   --dg-differs: WHAT    digres runs it and answers differently
#   --dg-crash: WHAT      digres crashed or hung on it
#   --dg-unstable         PostgreSQL itself answers it differently twice
import argparse
import difflib
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'regress'))
from split import split, CONDITIONALS  # noqa: E402
from report import normalize  # noqa: E402

DG = os.path.abspath(os.path.join(HERE, '..', '..'))
PG18 = os.environ.get('PG18_SRC', os.path.expanduser('~/p/postgres18'))
PG_BIN = os.path.join(PG18, 'install', 'bin')
PG_REGRESS = os.path.join(PG18, 'src', 'test', 'regress', 'pg_regress')
DLPATH = os.path.join(PG18, 'src', 'test', 'regress')
WORK = os.path.join(DG, 'build', 'test', 'pgregress')
ENC = 'latin-1'  # reads and writes every byte as it is
TIMEOUT = 180
HEADER = re.compile(r'^--dg-(skip|cascade|differs|crash|unstable)(?:: (.*))?$')
MARK = re.compile(r'^@@dg (\d+)@@$')
LOST = re.compile(r'server closed the connection unexpectedly|'
                  r'connection to server was lost|no connection to the server')
# Errors a statement meets on its own, not for what came before it: every
# statement that meets one is taken out at once.
INTRINSIC = re.compile(r'not supported|digres has no catalog tables|'
                       r'cannot be made: digres|unrecognized configuration '
                       r'parameter|^language "\?" does not exist')


def free_port():
    """A TCP port nothing listens on now."""
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


class server_t:
    """A server, PostgreSQL 18's or digres's, on a data directory of its
    own under dir, listening on a socket in a short directory."""

    def __init__(self, kind, dir):
        self.kind = kind
        self.dir = dir
        self.port = free_port()
        # A socket path over 107 bytes cannot be bound.
        self.sock = tempfile.mkdtemp(prefix='dgp.')
        self.proc = None
        os.makedirs(dir, exist_ok=True)

    def initdb(self):
        """Make the data directory."""
        data = os.path.join(self.dir, 'data')
        shutil.rmtree(data, ignore_errors=True)
        if self.kind == 'pg':
            cmd = [os.path.join(PG_BIN, 'initdb'), '-D', data, '-E', 'UTF8',
                   '--locale=C', '-A', 'trust', '--no-sync']
        else:
            cmd = [os.path.join(DG, 'build', 'initdb'), data]
        with open(os.path.join(self.dir, 'initdb.out'), 'w') as out:
            subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT,
                           check=True)

    def start(self):
        """Start the server, making its data directory first if there is
        none, and wait until it accepts connections."""
        data = os.path.join(self.dir, 'data')
        if not os.path.isdir(data):
            self.initdb()
        if self.kind == 'pg':
            cmd = [os.path.join(PG_BIN, 'postgres'), '-D', data, '-c',
                   "listen_addresses=", '-c', 'fsync=off']
        else:
            cmd = [os.path.join(DG, 'build', 'digres'), '-D', data]
        log = open(os.path.join(self.dir, 'log'), 'a')
        # Each server in a process group of its own, so a stop takes it all.
        self.proc = subprocess.Popen(
            cmd + ['-k', self.sock, '-p', str(self.port)], stdout=log,
            stderr=subprocess.STDOUT, start_new_session=True)
        # A server that does not come up is stopped, not left holding its
        # data directory.
        try:
            for _ in range(300):
                if self.ready():
                    return
                if self.proc.poll() is not None:
                    break
                time.sleep(0.1)
            raise RuntimeError('%s server did not start; see %s/log' %
                               (self.kind, self.dir))
        except BaseException:
            self.stop()
            raise

    def psql(self, sql, db='postgres', check=True):
        """Run sql in db; its output."""
        r = subprocess.run(
            [os.path.join(PG_BIN, 'psql'), '-X', '-q', '-At', '-h',
             self.sock, '-p', str(self.port), '-d', db, '-c', sql],
            capture_output=True, text=True, timeout=60)
        if check and r.returncode != 0:
            raise RuntimeError('%s: %s' % (sql, r.stderr))
        return r.stdout

    def ready(self):
        """Whether the server answers."""
        try:
            r = subprocess.run(
                [os.path.join(PG_BIN, 'pg_isready'), '-h', self.sock, '-p',
                 str(self.port), '-q'], timeout=10)
        except subprocess.TimeoutExpired:
            return False
        return r.returncode == 0

    def alive(self):
        """Whether the server is still running."""
        return self.proc is not None and self.proc.poll() is None

    def stop(self):
        """Stop the server: a fast shutdown, then a kill."""
        if not self.alive():
            return
        os.killpg(self.proc.pid, signal.SIGINT)
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait()

    def restart(self):
        """Start the server again after a crash or a hang, on a new data
        directory if it will not start on its own."""
        if self.alive():
            os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait()
        try:
            self.start()
        except RuntimeError:
            shutil.rmtree(os.path.join(self.dir, 'data'), ignore_errors=True)
            self.start()

    def fresh_database(self):
        """Make the database regression afresh, with nothing left of the
        last file: its databases, roles, tablespaces and sessions."""
        # A subscription holds its database: PostgreSQL's go first.
        if self.kind == 'pg':
            for sub in self.psql("select subname from pg_subscription",
                                 check=False).split():
                for sql in ('alter subscription %s disable',
                            'alter subscription %s set (slot_name = none)',
                            'drop subscription %s'):
                    self.psql(sql % sub, db='regression', check=False)
        self.psql('drop database if exists regression with (force)')
        sql = ("select format('drop database if exists %I with (force)', "
               "datname) from pg_database where datname like 'regression%' "
               "union all select format('drop role if exists %I', rolname) "
               "from pg_roles where rolname like 'regress%'")
        # digres has no tablespaces.
        if self.kind == 'pg':
            sql += (" union all select format('drop tablespace if exists %I',"
                    " spcname) from pg_tablespace where spcname like "
                    "'regress%'")
        drops = self.psql(sql)
        for d in drops.splitlines():
            self.psql(d, check=False)
        self.psql('create database regression')

    def run(self, name, text, setup):
        """Run the file name, whose text is given, after setup's text if
        there is one, in a fresh database; psql's output, and whether the
        run timed out."""
        if not self.alive() or not self.ready():
            self.restart()
        try:
            self.fresh_database()
        except (RuntimeError, subprocess.TimeoutExpired):
            self.restart()
            self.fresh_database()
        indir = os.path.join(self.dir, 'in')
        outdir = os.path.join(self.dir, 'out')
        shutil.rmtree(indir, ignore_errors=True)
        shutil.rmtree(outdir, ignore_errors=True)
        os.makedirs(os.path.join(indir, 'sql'))
        os.makedirs(os.path.join(indir, 'expected'))
        os.symlink(os.path.join(HERE, 'data'), os.path.join(indir, 'data'))
        names = (['test_setup'] if setup is not None else []) + [name]
        for n, t in zip(names, ([setup] if setup is not None else []) +
                        [text]):
            with open(os.path.join(indir, 'sql', n + '.sql'), 'w',
                      encoding=ENC) as f:
                f.write(t)
            open(os.path.join(indir, 'expected', n + '.out'), 'w').close()
        timed_out = False
        try:
            subprocess.run(
                [PG_REGRESS, '--use-existing', '--host=' + self.sock,
                 '--port=%d' % self.port, '--bindir=' + PG_BIN,
                 '--inputdir=' + indir, '--outputdir=' + outdir,
                 '--dlpath=' + DLPATH, '--dbname=regression'] + names,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                timeout=TIMEOUT, start_new_session=True)
        except subprocess.TimeoutExpired:
            timed_out = True
        try:
            with open(os.path.join(outdir, 'results', name + '.out'),
                      encoding=ENC) as f:
                out = f.read()
        except FileNotFoundError:
            out = ''
        return out, timed_out


def load(path):
    """A ported file's original text, PostgreSQL's with its statements
    back in, and what was taken out: {unit index: (kind, why)}."""
    with open(path, encoding=ENC) as f:
        lines = f.read().split('\n')
    orig = []
    blocks = []  # (start offset, end offset, kind, why) in orig
    pos = 0
    i = 0
    while i < len(lines):
        m = HEADER.match(lines[i])
        if not m:
            orig.append(lines[i])
            pos += len(lines[i]) + 1
            i += 1
            continue
        start = pos
        i += 1
        while i < len(lines) and (lines[i] == '--dg' or
                                  lines[i].startswith('--dg ')):
            line = lines[i][5:]
            orig.append(line)
            pos += len(line) + 1
            i += 1
        blocks.append((start, pos, m.group(1), m.group(2) or ''))
    text = '\n'.join(orig)
    units = split(text)
    off = {}
    for k, u in enumerate(units):
        for start, end, kind, why in blocks:
            if start <= u.start < end:
                off[k] = (kind, why)
    return text, units, off


def render(text, units, off, marks=False):
    """The file's text with the units in off commented out, and with a
    marker before each unit if marks."""
    out = []
    pos = 0
    for k, u in enumerate(units):
        gap = text[pos:u.start]
        # A unit alone on its line starts there, its indent and all, so
        # the marker and a header begin the line.
        begin = text.rfind('\n', 0, u.start) + 1
        alone = begin >= pos and text[begin:u.start].strip() == ''
        indent = text[begin:u.start] if alone else ''
        out.append(gap[:len(gap) - len(indent)])
        if (marks or k in off) and not alone:
            out.append('\n')
        if marks:
            out.append('\\echo @@dg %d@@\n' % k)
        body = indent + text[u.start:u.end]
        pos = u.end
        if k not in off:
            out.append(body)
            continue
        kind, why = off[k]
        out.append('--dg-%s%s\n' % (kind, ': ' + why if why else ''))
        lines = body.rstrip('\n').split('\n')
        out.append('\n'.join('--dg ' + l if l else '--dg' for l in lines))
        if body.endswith('\n'):
            out.append('\n')
            continue
        # What follows on the unit's last line: a comment stays on it,
        # another unit goes to a line of its own.
        nl = text.find('\n', u.end)
        rest = text[u.end:nl if nl >= 0 else len(text)].strip()
        if rest and not rest.startswith('--'):
            out.append('\n')
    out.append(text[pos:])
    if marks:
        out.append('\n\\echo @@dg %d@@\n' % len(units))
    return ''.join(out)


def blocks(out, n):
    """Each unit's output, by the markers in psql's output: a list of n,
    None for a unit whose marker never came."""
    got = [None] * n
    cur = None
    for line in out.split('\n'):
        m = MARK.match(line)
        if m:
            k = int(m.group(1))
            cur = k if k < n else None
            if cur is not None:
                got[cur] = []
            continue
        if line.lstrip().startswith('\\echo @@dg '):
            continue
        if cur is not None:
            got[cur].append(line)
    return [None if b is None else '\n'.join(b) for b in got]


def first_error(block):
    """The first error in a unit's output, or None."""
    for line in (block or '').split('\n'):
        m = re.match(r'^(?:psql:[^:]*:\d+: )?(?:ERROR|FATAL):\s+(.*)', line)
        if m:
            return m.group(1).strip()
    return None


def classify(pg, dg):
    """Why a unit's output on digres, dg, is not PostgreSQL's, pg: the
    kind and the reason to write."""
    if dg is None or LOST.search(dg):
        return 'crash', 'the connection was lost'
    de, pe = first_error(dg), first_error(pg)
    if de and not pe:
        return 'skip', normalize(de)
    if de and pe:
        return 'differs', 'another error: ' + normalize(de)
    if pe:
        return 'differs', 'no error where PostgreSQL has: ' + normalize(pe)
    if 'QUERY PLAN' in (pg or ''):
        return 'differs', 'the plan'
    return 'differs', 'the result'


class worker_t:
    """The servers, PostgreSQL's and digres's or digres's alone, that
    files are run on one at a time, under WORK/sub."""

    def __init__(self, n, kinds=('pg', 'dg'), sub=''):
        dir = os.path.join(WORK, sub, 'w%d' % n)
        self.pg = self.dg = None
        try:
            for kind in kinds:
                s = server_t(kind, os.path.join(dir, kind))
                s.start()
                setattr(self, kind, s)
        except BaseException:
            self.stop()
            raise

    def stop(self):
        for s in (self.pg, self.dg):
            if s:
                s.stop()


def setup_text(name):
    """test_setup as it is ported, to run before name; None for
    test_setup itself."""
    if name == 'test_setup':
        return None
    text, units, off = load(os.path.join(HERE, 'sql', 'test_setup.sql'))
    return render(text, units, off)


def log(msg):
    """Say what was done, a line at a time."""
    sys.stdout.write(msg + '\n')
    sys.stdout.flush()


def trim(w, name):
    """Take out of name every statement digres does not run as
    PostgreSQL does, a pass at a time, until a pass takes out none."""
    path = os.path.join(HERE, 'sql', name + '.sql')
    text, units, off = load(path)
    n = len(units)
    setup = setup_text(name)
    keep = {k for k, u in enumerate(units)
            if u.kind == 'meta' and u.cmd in CONDITIONALS}
    # PostgreSQL's own answers, with nothing taken out, run twice: a unit
    # answered differently each time is unstable.
    inst = render(text, units, {}, marks=True)
    ref = blocks(w.pg.run(name, inst, setup)[0], n)
    again = blocks(w.pg.run(name, inst, setup)[0], n)
    for k in range(n):
        if ref[k] != again[k] and k not in off and k not in keep:
            off[k] = ('unstable', '')
    passes = 0
    while True:
        passes += 1
        inst = render(text, units, off, marks=True)
        pg = blocks(w.pg.run(name, inst, setup)[0], n)
        out, timed_out = w.dg.run(name, inst, setup)
        dg = blocks(out, n)
        crashed = not w.dg.alive() or timed_out
        took = {}
        for k in range(n):
            if k in off or k in keep:
                continue
            if pg[k] != ref[k]:
                took[k] = ('cascade', '')
        first = True
        for k in range(n):
            if k in off or k in keep or k in took or pg[k] == dg[k]:
                continue
            if dg[k] is None and crashed:
                # The unit before the first with no marker is the one the
                # server died or hung on.
                j = max([i for i in range(k) if dg[i] is not None and
                         i not in off and i not in keep], default=None)
                if j is not None and j not in took:
                    took[j] = ('crash', 'a timeout' if timed_out else
                               'the server crashed')
                break
            kind, why = classify(pg[k], dg[k])
            if first or (kind == 'skip' and INTRINSIC.search(why)):
                took[k] = (kind, why)
                first = False
        if crashed:
            w.dg.restart()
        if not took:
            break
        off.update(took)
        with open(path, 'w', encoding=ENC) as f:
            f.write(render(text, units, off))
    counts = {}
    for kind, _ in off.values():
        counts[kind] = counts.get(kind, 0) + 1
    log('%-28s %5d units, %4d taken out in %3d passes %s' %
        (name, n, len(off), passes, counts))


def expected(w, name):
    """Write expected/name.out: the ported file's output on
    PostgreSQL 18."""
    with open(os.path.join(HERE, 'sql', name + '.sql'), encoding=ENC) as f:
        text = f.read()
    out, _ = w.pg.run(name, text, setup_text(name))
    os.makedirs(os.path.join(HERE, 'expected'), exist_ok=True)
    with open(os.path.join(HERE, 'expected', name + '.out'), 'w',
              encoding=ENC) as f:
        f.write(out)
    log('%-28s expected written' % name)


def test(w, name):
    """Run name on digres and compare its output with expected/name.out,
    blind to white space as pg_regress is; whether they agree. The output
    and a diff go in WORK/test."""
    with open(os.path.join(HERE, 'sql', name + '.sql'), encoding=ENC) as f:
        text = f.read()
    out, timed_out = w.dg.run(name, text, setup_text(name))
    with open(os.path.join(HERE, 'expected', name + '.out'),
              encoding=ENC) as f:
        want = f.read()
    dir = os.path.join(WORK, 'test')
    with open(os.path.join(dir, name + '.out'), 'w', encoding=ENC) as f:
        f.write(out)

    def norm(t):
        return [re.sub(r'\s+', '', line) for line in t.split('\n')]

    ok = not timed_out and norm(out) == norm(want)
    diff = os.path.join(dir, name + '.diff')
    if ok:
        if os.path.exists(diff):
            os.unlink(diff)
    else:
        with open(diff, 'w', encoding=ENC) as f:
            f.writelines(difflib.unified_diff(
                want.splitlines(True), out.splitlines(True),
                'expected/' + name + '.out', 'results/' + name + '.out'))
    log('%-28s %s' % (name, 'ok' if ok else
                      'TIMEOUT' if timed_out else 'FAILED'))
    return ok


def check(w, name):
    """Check split.py on name: PostgreSQL's output with a marker before
    each unit must be its output without them, every marker there but
    those a \\quit or a false \\if passes over."""
    with open(os.path.join(HERE, 'sql', name + '.sql'), encoding=ENC) as f:
        text = f.read()
    units = split(text)
    plain = w.pg.run(name, text, None)[0]
    inst = w.pg.run(name, render(text, units, {}, marks=True), None)[0]
    got = [int(m.group(1)) for m in
           (MARK.match(l) for l in inst.split('\n')) if m]
    missing = sorted(set(range(len(units) + 1)) - set(got))
    strip = [l for l in inst.split('\n') if not MARK.match(l) and
             not l.lstrip().startswith('\\echo @@dg ')]
    # The same words, in any order: psql echoes a line once, so a line of
    # several units, split by the markers, echoes in another place.
    same = sorted(re.findall(r'\S+', '\n'.join(strip))) == \
        sorted(re.findall(r'\S+', plain))
    log('%-28s %5d units %s%s' % (
        name, len(units), 'same' if same else 'DIFFERS',
        ', markers missing: %s' % missing[:10] if missing else ''))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-j', type=int, default=4)
    ap.add_argument('what', choices=['trim', 'expected', 'check', 'test'])
    ap.add_argument('files', nargs='*')
    a = ap.parse_args()
    # A tree built but not installed into install/ has no client to run.
    if not all(os.access(p, os.X_OK) for p in
               (PG_REGRESS, os.path.join(PG_BIN, 'pg_isready'))):
        log('pgregress: SKIPPED (no PostgreSQL 18 build in %s; set '
            'PG18_SRC)' % PG18)
        return 0
    with open(os.path.join(HERE, 'schedule')) as f:
        schedule = f.read().split()
    files = a.files or schedule
    fn = {'trim': trim, 'expected': expected, 'check': check,
          'test': test}[a.what]
    kinds = {'trim': ('pg', 'dg'), 'test': ('dg',)}.get(a.what, ('pg',))
    sub = a.what if a.what != 'trim' else ''
    os.makedirs(os.path.join(WORK, sub), exist_ok=True)
    failed = []
    # A file trimmed runs after test_setup as it is trimmed: test_setup
    # first, alone.
    if a.what == 'trim' and 'test_setup' in files:
        files = [f for f in files if f != 'test_setup']
        w = worker_t(0, kinds, sub)
        try:
            fn(w, 'test_setup')
        finally:
            w.stop()
    if not files:
        return 0
    workers = []
    free = []
    lock = threading.Lock()

    def one(name):
        with lock:
            w = free.pop()
        try:
            if fn(w, name) is False:
                failed.append(name)
        except Exception as e:
            log('%-28s FAILED: %s' % (name, e))
            failed.append(name)
        finally:
            with lock:
                free.append(w)

    # Workers started before one that fails to start are stopped too.
    try:
        for i in range(min(a.j, len(files))):
            workers.append(worker_t(i, kinds, sub))
        free.extend(workers)
        with ThreadPoolExecutor(len(workers)) as ex:
            list(ex.map(one, files))
    finally:
        for w in workers:
            w.stop()
    if failed:
        log('%d of %d failed: %s' % (len(failed), len(files),
                                     ' '.join(sorted(failed))))
        return 1
    log('all %d ok' % len(files))
    return 0


if __name__ == '__main__':
    sys.exit(main())
