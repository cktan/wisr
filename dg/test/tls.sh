# Verify TLS as ../aa/ROLE.md section 8 sets it: with ssl on, a client over
# TCP uses TLS and the local socket never does; the certificate served is
# the one configured, taken again on a reload; and many sessions, each a
# thread, make their connections from one context while it is replaced.
# 1. On server.sh's server, ssl off: sslmode=require is refused, prefer
#    connects in the clear.
# 2. Make a CA, and two server certificates for localhost it signs.
# 3. Start a server with ssl on and the first certificate.
# 4. sslmode=require connects over TLS; verify-full with the CA connects to
#    localhost and refuses 127.0.0.1, a name the certificate lacks.
# 5. Over TCP without TLS a client is refused; the local socket connects.
# 6. SCRAM's channel binding is taken; a password in cleartext is set over
#    TLS; direct TLS, with ALPN, connects, given a client of 17 or later.
# 7. 16 sessions connect over TLS at once, again and again, while the
#    configuration is reloaded: every one answers.
# 8. A session over TLS is held; the second certificate is configured and
#    reloaded: a new connection is served it, and the held session answers.
# 9. A reload with no key file keeps the second certificate, and says so.
# 10. A server whose key file is missing does not start.
# Sourced by server.sh, using its server, port, fail and server_ready.
tw=$work/tls
mkdir -p "$tw"
# A psql whose libpq has TLS: the one on PATH, else Ubuntu's; one built
# without it cannot ask for TLS ("SSL support is not compiled in").
no_tls() {
  "$1" "host=localhost port=1 sslmode=require" 2>&1 |
    grep -q 'SSL support is not compiled in'
}
tpsql=psql
no_tls "$tpsql" && tpsql=/usr/bin/psql
if [ ! -x "$(command -v "$tpsql")" ] || no_tls "$tpsql"; then
  fail "tls: no psql with TLS; build PostgreSQL 18 --with-ssl=openssl"
  return
fi

# 1. ssl off, on server.sh's server.
PGPASSWORD=sesame "$tpsql" -X -A -t \
  "host=127.0.0.1 port=$port dbname=postgres sslmode=require" \
  -c 'select 1' > "$tw/off.out" 2>&1 &&
  fail "tls: sslmode=require taken with ssl off"
grep -q 'server does not support SSL' "$tw/off.out" ||
  fail "tls: sslmode=require with ssl off says why"
[ "$(PGPASSWORD=sesame "$tpsql" -X -A -t \
  "host=127.0.0.1 port=$port dbname=postgres sslmode=prefer" \
  -c 'select 1' 2> "$tw/prefer.err")" = 1 ] ||
  fail "tls: sslmode=prefer with ssl off"

# 2. A CA, and two certificates for localhost it signs; a key file must be
#    the owner's alone.
openssl req -x509 -newkey rsa:2048 -nodes -days 30 -subj "/CN=wisr test CA" \
  -keyout "$tw/ca.key" -out "$tw/ca.crt" > "$tw/openssl.log" 2>&1
for s in s1 s2; do
  openssl req -newkey rsa:2048 -nodes -subj "/O=$s/CN=localhost" \
    -keyout "$tw/$s.key" -out "$tw/$s.csr" >> "$tw/openssl.log" 2>&1
  printf 'subjectAltName=DNS:localhost\n' > "$tw/$s.ext"
  openssl x509 -req -in "$tw/$s.csr" -CA "$tw/ca.crt" -CAkey "$tw/ca.key" \
    -CAcreateserial -days 30 -extfile "$tw/$s.ext" -out "$tw/$s.crt" \
    >> "$tw/openssl.log" 2>&1
  chmod 600 "$tw/$s.key"
done
[ -s "$tw/s1.crt" ] && [ -s "$tw/s2.crt" ] || fail "tls: certificates made"

# 3. A server with ssl on.
tport=$(python3 -c 'import socket; s = socket.socket()
s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
build/initdb --pwfile="$work/pw" "$tw/data" > "$tw/initdb.out" 2>&1 ||
  fail "tls: initdb"
# In postgresql.conf, not on the command line, which ALTER SYSTEM cannot
# override, so that a reload can change them (8 below).
printf "ssl = on\nssl_cert_file = '%s'\nssl_key_file = '%s'\n" \
  "$tw/s1.crt" "$tw/s1.key" >> "$tw/data/postgresql.conf"
$digres -D "$tw/data" -k "$tw" -p "$tport" > "$tw/log" 2>&1 &
tserver=$!
server_ready $tserver "$tw/log" || fail "tls: start with ssl on"

# A client over TCP, with the connection options given, as the superuser.
tls_psql() {
  PGPASSWORD=sesame "$tpsql" -X -A -t \
    "host=localhost port=$tport dbname=postgres $1" "${@:2}"
}

# 4. TLS, and the certificate it is.
tls_psql "sslmode=require" -c '\conninfo' > "$tw/conninfo.out" 2>&1
grep -qi 'SSL Connection|true\|SSL connection (protocol: TLS' "$tw/conninfo.out" ||
  fail "tls: sslmode=require is over TLS"
[ "$(tls_psql "sslmode=verify-full sslrootcert=$tw/ca.crt" \
  -c 'select 1' 2> "$tw/verify.err")" = 1 ] ||
  fail "tls: verify-full with the CA"
PGPASSWORD=sesame "$tpsql" -X -A -t "host=127.0.0.1 port=$tport dbname=postgres \
  sslmode=verify-full sslrootcert=$tw/ca.crt" -c 'select 1' \
  > "$tw/wrong.out" 2>&1 && fail "tls: verify-full of a name not served"
grep -q 'does not match host name' "$tw/wrong.out" ||
  fail "tls: verify-full of a name not served says why"

# 5. TCP without TLS, and the local socket.
tls_psql "sslmode=disable" -c 'select 1' > "$tw/plain.out" 2>&1 &&
  fail "tls: TCP without TLS taken with ssl on"
grep -q 'a connection over TCP must use SSL' "$tw/plain.out" ||
  fail "tls: TCP without TLS says why"
[ "$(psql -X -A -t -h "$tw" -p "$tport" -d postgres -c 'select 1' \
  2> "$tw/local.err")" = 1 ] || fail "tls: the local socket, no TLS"

# 6. Channel binding, a password in cleartext, and direct TLS.
[ "$(tls_psql "sslmode=require channel_binding=require" -c 'select 1' \
  2> "$tw/binding.err")" = 1 ] || fail "tls: channel binding"
psql -X -A -t -h "$tw" -p "$tport" -d postgres \
  -c 'create role tlsr login' > /dev/null 2>&1
tls_psql "sslmode=require" -c "alter role tlsr password 'pt'" \
  > "$tw/cleartext.out" 2>&1 || fail "tls: a password in cleartext over TLS"
[ "$(PGPASSWORD=pt "$tpsql" -X -A -t "host=localhost port=$tport \
  dbname=postgres user=tlsr sslmode=require" -c 'select current_user' \
  2> "$tw/tlsr.err")" = tlsr ] || fail "tls: the password set over TLS"
# Direct TLS needs a client of PostgreSQL 17 or later; else it is skipped.
if [ "$("$tpsql" --version | sed -E 's/[^0-9]*([0-9]+).*/\1/')" -ge 17 ]; then
  PGPASSWORD=sesame "$tpsql" -X -A -t "host=localhost port=$tport \
    dbname=postgres sslmode=require sslnegotiation=direct" -c '\conninfo' \
    > "$tw/direct.out" 2>&1
  grep -qi 'SSL Connection|true\|SSL connection (protocol' "$tw/direct.out" ||
    fail "tls: direct TLS"
fi

# 7. 16 sessions at once over TLS, while the configuration is reloaded.
(for _ in $(seq 40); do
   psql -X -A -t -h "$tw" -p "$tport" -d postgres \
     -c 'select pg_reload_conf()' > /dev/null 2>&1
   sleep 0.05
 done) &
reloader=$!
pids=()
for i in $(seq 16); do
  (for _ in $(seq 5); do
     tls_psql "sslmode=require" -c 'select count(*) from generate_series(1, 1000)'
   done) > "$tw/many$i.out" 2>&1 &
  pids+=($!)
done
wait "${pids[@]}"
wait $reloader
[ "$(cat "$tw"/many*.out | grep -cx 1000)" = 80 ] ||
  fail "tls: 16 sessions at once, while reloaded"

# The subject of the certificate a new connection is served.
served() {
  openssl s_client -starttls postgres -connect "localhost:$tport" \
    < /dev/null 2> /dev/null | openssl x509 -noout -subject 2> /dev/null
}

# 8. A held session across a reload to the second certificate.
grep -q 'O *= *s1' <(served) || fail "tls: the first certificate served"
mkfifo "$tw/held.in"
PGPASSWORD=sesame "$tpsql" -X -A -t "host=localhost port=$tport dbname=postgres \
  sslmode=require" < "$tw/held.in" > "$tw/held.out" 2>&1 &
heldpid=$!
exec {heldfd}> "$tw/held.in"
echo "select 'before';" >&$heldfd
psql -X -A -t -h "$tw" -p "$tport" -d postgres \
  -c "alter system set ssl_cert_file = '$tw/s2.crt'" \
  -c "alter system set ssl_key_file = '$tw/s2.key'" \
  -c 'select pg_reload_conf()' > "$tw/reload.out" 2>&1
sleep 0.5
grep -q 'O *= *s2' <(served) || fail "tls: the second certificate, reloaded"
echo "select 'after';" >&$heldfd
exec {heldfd}>&-
wait $heldpid
grep -qx after "$tw/held.out" || fail "tls: a session held across a reload"

# 9. A reload with no key file keeps what it had.
psql -X -A -t -h "$tw" -p "$tport" -d postgres \
  -c "alter system set ssl_key_file = '$tw/none.key'" \
  -c 'select pg_reload_conf()' > /dev/null 2>&1
sleep 0.5
grep -q 'SSL configuration was not reloaded' "$tw/log" ||
  fail "tls: a bad reload says so"
grep -q 'O *= *s2' <(served) || fail "tls: a bad reload keeps the certificate"

kill -INT $tserver
wait $tserver || fail "tls: shutdown status"

# 10. A server whose key file is missing does not start.
$digres -D "$tw/data" -k "$tw" -p "$tport" -c ssl=on \
  -c ssl_cert_file="$tw/s1.crt" -c ssl_key_file="$tw/none.key" \
  > "$tw/nokey.log" 2>&1 &
nokey=$!
if server_ready $nokey "$tw/nokey.log"; then
  fail "tls: a server with no key file started"
  kill -INT $nokey
fi
wait $nokey 2> /dev/null
grep -q 'could not access private key file' "$tw/nokey.log" ||
  fail "tls: a missing key file says why"
