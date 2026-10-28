#!/bin/bash
# initdb.sh -- check that initdb makes a data directory, and refuses
# what digres does not support before making anything.
#
#   test/initdb.sh
#
# Each check's output is left in build/test/initdb/.
cd "$(dirname "$0")/.." || exit 1
work=build/test/initdb
rm -rf "$work"
mkdir -p "$work"
initdb=build/initdb
failed=0

# Report the check NAME as failed.
fail() {
  echo "initdb: FAILED ($1; see $work)"
  failed=1
}

# Verify that initdb makes a data directory the server can use.
# 1. Run initdb on a directory that does not exist.
# 2. Confirm the version file and the storage, and that no superuser file
#    is written: the roles are in the database pg_global.
# 3. Confirm no one but the owner can enter the directory.
$initdb -D "$work/plain" > "$work/plain.out" 2>&1 || fail "plain run"
[ "$(cat "$work/plain/PG_VERSION")" = 18 ] || fail "PG_VERSION"
[ -d "$work/plain/global" ] && [ -d "$work/plain/base" ] || fail "storage"
[ -e "$work/plain/superuser" ] && fail "a superuser file"
[ "$(stat -c %a "$work/plain")" = 700 ] || fail "permissions"

# Verify that the server's own name makes one too, in an empty directory.
# 1. Make the directory.
# 2. Run "digres --init" with the directory as its argument.
# 3. Confirm the version file is there.
mkdir "$work/empty"
build/digres --init "$work/empty" > "$work/empty.out" 2>&1 || fail "--init"
[ -f "$work/empty/PG_VERSION" ] || fail "--init PG_VERSION"

# Verify that -U names the superuser and --pwfile gives it a password,
# both kept in pg_global, which the server reads at its start.
# 1. Write a password file.
# 2. Run initdb with both options, and the encoding and locale allowed.
# 3. Start the server on it, and connect over TCP as alice, by password.
# 4. Confirm pg_authid has alice as oid 10, the superuser, with a SCRAM
#    secret, not the password, kept.
# 5. Make alice NOLOGIN, CONNECTION LIMIT 0 and past VALID UNTIL: the
#    data directory's owner, the OS user, still gets in as alice on the
#    local socket, by peer (sm/ROLE.md section 5, "Locked out"); eve,
#    another role, does not without its password.
echo "secret" > "$work/pw"
$initdb -D "$work/named" -U alice --pwfile="$work/pw" -E UTF8 \
  --locale=C.UTF-8 > "$work/named.out" 2>&1 || fail "named run"
if command -v psql > /dev/null; then
  port=${DG_TEST_PORT:-$(python3 -c 'import socket; s = socket.socket()
s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')}
  build/digres -D "$work/named" -k "$PWD/$work" -p "$port" \
    > "$work/named.log" 2>&1 &
  server=$!
  for _ in $(seq 300); do
    grep -q "ready to accept connections" "$work/named.log" && break
    sleep 0.1
  done
  PGPASSWORD=secret psql -X -A -t -h 127.0.0.1 -p "$port" -U alice \
    -d postgres -c "select oid, rolname, rolsuper,
                      rolpassword ~ '^SCRAM-SHA-256\\$',
                      rolpassword ~ 'secret'
                    from pg_authid where oid = 10" > "$work/named.authid" 2>&1
  PGPASSWORD=secret psql -X -A -t -h 127.0.0.1 -p "$port" -U alice \
    -d postgres -c "alter role alice nologin connection limit 0
                      valid until '2000-01-01'" -c 'create role eve login' \
    > "$work/named.alter" 2>&1
  psql -X -A -t -w -h "$PWD/$work" -p "$port" -U alice -d postgres \
    -c 'select current_user' > "$work/named.owner" 2>&1
  psql -X -A -t -w -h "$PWD/$work" -p "$port" -U eve -d postgres \
    -c 'select current_user' > "$work/named.eve" 2>&1 &&
    fail "another role by the owner's peer"
  kill $server
  wait $server
  [ "$(cat "$work/named.authid")" = "10|alice|t|t|f" ] ||
    fail "-U and --pwfile in pg_authid"
  [ "$(cat "$work/named.owner")" = alice ] ||
    fail "the data directory's owner as the bootstrap superuser"
fi

# Verify that a data directory made before digres kept roles, with no
# pg_global, is refused at the server's start, and told to run initdb.
# 1. Make a data directory, and drop its pg_global with sm's driver.
# 2. Start the server on it, and confirm it exits with that message, at
#    its start, before it listens.
$initdb -D "$work/old" > "$work/old.out" 2>&1 || fail "old run"
printf '(env-attach "%s")\n(db-drop "pg_global")\n' "$PWD/$work/old" \
  > "$work/old.rel"
../sm/build/rel "$work/old.rel" > "$work/old.rel.out" 2>&1 ||
  fail "drop pg_global"
build/digres -D "$work/old" -k "$PWD/$work" -p 1 > "$work/old.log" 2>&1
grep -q 'data directory has no database "pg_global"' "$work/old.log" &&
  grep -q 'Run initdb to make a new data directory' "$work/old.log" ||
  fail "no pg_global message"

# Verify that a directory holding anything is left alone.
# 1. Run initdb on the directory made above.
# 2. Confirm it fails.
$initdb -D "$work/plain" > "$work/again.out" 2>&1 && fail "non-empty"

# Verify that what digres does not support is refused, and nothing made.
# 1. Run initdb with each such option on a directory that does not exist.
# 2. Confirm each run fails.
# 3. Confirm the directory was not made.
n=0
for opts in "-E LATIN1" "--locale=en_US.UTF-8" "--auth=trust" "-A trust" \
  "-k" "--no-sync" "-X /tmp/wal" "--pwfile=$work/none" "-U pg_x"; do
  n=$((n + 1))
  # shellcheck disable=SC2086
  $initdb -D "$work/refused" $opts > "$work/refused$n.out" 2>&1 &&
    fail "accepted $opts"
  [ -e "$work/refused" ] && fail "made a directory for $opts"
done

[ $failed = 0 ] && echo "initdb: PASSED"
exit $failed
