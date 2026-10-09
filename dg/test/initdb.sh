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
# 2. Confirm the version file, the storage and the superuser's name.
# 3. Confirm no one but the owner can enter the directory.
$initdb -D "$work/plain" > "$work/plain.out" 2>&1 || fail "plain run"
[ "$(cat "$work/plain/PG_VERSION")" = 18 ] || fail "PG_VERSION"
[ -d "$work/plain/global" ] && [ -d "$work/plain/base" ] || fail "storage"
[ "$(head -1 "$work/plain/superuser")" = "$(id -un)" ] || fail "superuser"
[ "$(stat -c %a "$work/plain")" = 700 ] || fail "permissions"

# Verify that the server's own name makes one too, in an empty directory.
# 1. Make the directory.
# 2. Run "digres --init" with the directory as its argument.
# 3. Confirm the version file is there.
mkdir "$work/empty"
build/digres --init "$work/empty" > "$work/empty.out" 2>&1 || fail "--init"
[ -f "$work/empty/PG_VERSION" ] || fail "--init PG_VERSION"

# Verify that -U names the superuser and --pwfile gives it a password.
# 1. Write a password file.
# 2. Run initdb with both options, and the encoding and locale allowed.
# 3. Confirm the name, and that a SCRAM secret, not the password, is kept.
echo "secret" > "$work/pw"
$initdb -D "$work/named" -U alice --pwfile="$work/pw" -E UTF8 \
  --locale=C.UTF-8 > "$work/named.out" 2>&1 || fail "named run"
[ "$(sed -n 1p "$work/named/superuser")" = alice ] || fail "-U"
sed -n 2p "$work/named/superuser" | grep -q '^SCRAM-SHA-256\$' ||
  fail "SCRAM secret"
grep -q "^secret" "$work/named/superuser" && fail "password kept"

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
