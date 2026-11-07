# Verify that COPY into an empty B-tree detects a committed reader's SSI edge.
# 1. Make an empty indexed table and a second table.
# 2. The loader reads the second table at SERIALIZABLE.
# 3. Another transaction scans the index.
# 4. Insert into the second table in that transaction.
# 5. Commit that transaction.
# 6. COPY a row into the indexed table.
# 7. Require the loader's commit to fail serialization.
# 8. Confirm that the aborted COPY leaves no row.
# Sourced by server.sh, using its held-session helpers.
for kind in plain unique; do
  clause=''
  [ "$kind" = unique ] && clause=unique
  local_psql -v ON_ERROR_STOP=1 -c "
    create table tls_x(v int);
    create $clause index tls_ix on tls_x using btree(v);
    create table tls_y(v int);" > "$work/tls-$kind.log" 2>&1 ||
    fail "tree load SSI: $kind setup"
  held_start "tls_$kind"
  held_send "tls_$kind" "begin isolation level serializable;
    select count(*) from tls_y; select 'loader ready';"
  held_wait "tls_$kind" 'loader ready' || fail "tree load SSI: loader read"
  local_psql -v ON_ERROR_STOP=1 -c "
    begin isolation level serializable;
    set local enable_seqscan=off;
    set local enable_bitmapscan=off;
    select count(*) from tls_x where v=1;
    insert into tls_y values(1);
    commit;" >> "$work/tls-$kind.log" 2>&1 ||
    fail "tree load SSI: reader commit"
  held_send "tls_$kind" "copy tls_x from stdin;
1
\.
commit;
select 'loader done';"
  held_wait "tls_$kind" 'loader done' || fail "tree load SSI: loader commit"
  grep -q 'not serializable' "$work/tls_$kind.out" ||
    fail "tree load SSI: $kind load committed"
  held_end "tls_$kind"
  [ "$(local_psql -c 'select count(*) from tls_x')" = 0 ] ||
    fail "tree load SSI: aborted rows are visible"
  local_psql -c 'drop table tls_x, tls_y' >> "$work/tls-$kind.log" 2>&1
done
