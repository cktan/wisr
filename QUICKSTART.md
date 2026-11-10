# Quick Start

From a fresh checkout to a query, in six steps: build digres,
make a database, start the server, connect with `psql`, create a
table, and read it back. [`DG-vs-PG.md`](DG-vs-PG.md) says what
digres is, and how it differs from PostgreSQL.

The steps are for Ubuntu 24.04. [`README.md`](README.md) has the
detail of each step, and the other platforms.

## 1. Build

Install the prerequisites once. `./SETUP_UBUNTU_DEV.sh` does it
on a minimal Ubuntu machine; [`README.md`](README.md),
"Prerequisites", lists the packages to install by hand.

From the top of the repository, build the debug build:

```
export DEBUG=1
make -j4
```

The server is `dg/build/digres`. The debug build links the
system's RocksDB, so it needs no other download.

## 2. Make a data directory

```
dg/build/digres --init ~/wisr-data
```

This makes the database `postgres`, with one superuser: your
OS user name. A data directory goes on a real disk, not in
`/tmp`.

## 3. Start the server

In a terminal of its own:

```
dg/build/digres -D ~/wisr-data -p 5432 -k /tmp
```

The server runs in the foreground and logs to the terminal. It
is ready when it logs `database system is ready to accept
connections`. Ctrl-C stops it.

If a PostgreSQL server already uses port 5432, give another
port, such as `-p 5433`, here and in step 4.

## 4. Connect with psql

In another terminal:

```
psql -h /tmp -p 5432 -d postgres
```

`-h /tmp` connects by the local socket, as your OS user, with
no password. Use PostgreSQL 18's `psql`; an older one connects
too, but some of its `\` commands fail.

## 5. Create a table

```sql
CREATE TABLE fruit (id int PRIMARY KEY, name text, price numeric(6,2));
INSERT INTO fruit VALUES (1, 'apple', 0.50), (2, 'banana', 0.25), (3, 'cherry', 4.00);
```

```
CREATE TABLE
INSERT 0 3
```

## 6. Read it back

```sql
SELECT * FROM fruit;
SELECT name, price FROM fruit WHERE price < 1 ORDER BY price;
```

```
 id |  name  | price
----+--------+-------
  1 | apple  |  0.50
  2 | banana |  0.25
  3 | cherry |  4.00
(3 rows)

  name  | price
--------+-------
 banana |  0.25
 apple  |  0.50
(2 rows)
```

## Next

- **What works and what does not**: [`DG-vs-PG.md`](DG-vs-PG.md),
  and the full list in [`dg/UNSUPPORTED.md`](dg/UNSUPPORTED.md).
  A statement digres does not run fails with an error, not with a
  wrong answer.
- **More databases**: `CREATE DATABASE app` makes one more; each
  is a directory of its own, `base/app`.
- **Connect over TCP**: give the superuser a password at step
  2 (`--init -W`), then `psql -h localhost -p 5432 -U NAME -d
  postgres`.
- **The release build, the tests and the benchmark**:
  [`README.md`](README.md).
