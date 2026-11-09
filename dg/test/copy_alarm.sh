# Verify that a stale timer does not end a bulk load: a timeout disabled
# leaves no ALARM armed, which in a COPY's final sort would fail the load
# (timeout.c's cancel_alarm(), ../ISSUE.md).
# 1. Time a COPY of 100,000 rows into an empty keyed table, a bulk load.
# 2. In one session, run SELECT 1 under a statement_timeout of d ms, clear
#    it, and COPY into an empty table: the timeout's timer, were it left,
#    would fire d ms on, with no timeout active.
# 3. The same inside a transaction under a transaction_timeout, which
#    stays active: the timer, were it left, would fire with a timeout
#    still active.
# 4. Each for 16 values of d across the last half of the COPY's time, where
#    its final sort runs: every COPY loads its rows.
# Sourced by server.sh, using its server, local_psql and fail.
seq 1 100000 | awk '{ print $1 "\t" $1 % 1000 "\trow" $1 }' > "$work/ca.tsv"
local_psql -c 'create table ca0 (a int4 primary key, b int4, c text)' \
  > "$work/ca.out" 2>&1
t0=$(date +%s%N)
local_psql -c "copy ca0 from '$work/ca.tsv'" >> "$work/ca.out" 2>&1
copy_ms=$((($(date +%s%N) - t0) / 1000000))
step=$((copy_ms / 32 + 1))
for how in alone in_transaction; do
  for i in $(seq 16); do
    d=$((copy_ms / 2 + i * step))
    t="ca_${how}_$i"
    if [ "$how" = alone ]; then
      out=$(local_psql -c "create table $t (a int4 primary key, b int4, c text)" \
        -c "set statement_timeout = '${d}ms'" -c 'select 1' \
        -c 'set statement_timeout = 0' -c "copy $t from '$work/ca.tsv'" 2>&1)
    else
      out=$(local_psql -c "create table $t (a int4 primary key, b int4, c text)" \
        -c "set transaction_timeout = '60s'" -c 'begin' \
        -c "set statement_timeout = '${d}ms'" -c 'select 1' \
        -c 'set statement_timeout = 0' -c "copy $t from '$work/ca.tsv'" \
        -c 'commit' 2>&1)
    fi
    echo "$how $d: $out" >> "$work/ca.out"
    grep -q 'COPY 100000' <<< "$out" ||
      fail "copy alarm: a stale timer, $how, at ${d}ms of ${copy_ms}ms"
  done
done
