# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# Verify that SERIALIZABLE lets one of two students take a class's last seat.
# The statement is the soak's add_class (bench/soak/add_class.sql): a count
# of the class's rows by its index, and an insert ON CONFLICT DO NOTHING.
# 1. Make enrollment and class, with an index on (class_id, student_id),
#    once by btree and once by lsm; a class holds 3, and has 2.
# 2. Two held sessions each add a student to class 1, and do not commit.
# 3. Commit both: the first commits, the second fails serialization.
# 4. Two held sessions add students to classes 2 and 3, one each.
# 5. Commit both: both commit, as the classes are different keys.
# 6. Require each class to hold 3 rows.
# Sourced by server.sh, using its held-session helpers.

# Print the add_class statement for student $1 and class $2, at SERIALIZABLE,
# with the count made to read the index, as it does on the soak's table.
seat_add() {
  echo "begin isolation level serializable;
    set local enable_seqscan = off;
    set local enable_bitmapscan = off;
    insert into seat_enr
      select $1, cl.id, null from seat_class cl
      where cl.id = $2
        and (select count(*) from seat_enr where class_id = cl.id)
            < cl.capacity
      on conflict do nothing;
    select 'added $1';"
}

for kind in btree lsm; do
  local_psql -v ON_ERROR_STOP=1 -c "
    create table seat_class(id int8 primary key, capacity int not null);
    create table seat_enr(student_id int8 not null, class_id int8 not null,
      grade int, primary key (student_id, class_id));
    create index seat_enr_class on seat_enr using $kind (class_id, student_id);
    insert into seat_class values (1, 3), (2, 3), (3, 3);
    insert into seat_enr values (1, 1, null), (2, 1, null),
      (3, 2, null), (4, 2, null), (5, 3, null), (6, 3, null);" > "$work/seat-$kind.log" 2>&1 ||
    fail "seat SSI: $kind setup"
  for round in same different; do
    # The students and classes of the two adds.
    s1=10 s2=11 c1=1 c2=1
    [ "$round" = different ] && s1=20 s2=21 c1=2 c2=3
    a="seat_${kind}_${round}_a"
    b="seat_${kind}_${round}_b"
    held_start "$a"
    held_start "$b"
    held_send "$a" "$(seat_add $s1 $c1)"
    held_wait "$a" "added $s1" ||
      fail "seat SSI: $kind $round, the first add"
    held_send "$b" "$(seat_add $s2 $c2)"
    held_wait "$b" "added $s2" ||
      fail "seat SSI: $kind $round, the second add"
    held_send "$a" "commit; select 'done a';"
    held_wait "$a" 'done a' || fail "seat SSI: $kind $round, the first commit"
    held_send "$b" "commit; select 'done b';"
    held_wait "$b" 'done b' || fail "seat SSI: $kind $round, the second commit"
    held_end "$a"
    held_end "$b"
    ! grep -q ERROR "$work/$a.out" ||
      fail "seat SSI: $kind $round, the first add failed"
    if [ "$round" = same ]; then
      grep -q 'not serializable\|could not serialize' "$work/$b.out" ||
        fail "seat SSI: $kind, two students took the last seat"
    else
      ! grep -q ERROR "$work/$b.out" ||
        fail "seat SSI: $kind, adds to two classes conflicted"
    fi
  done
  [ "$(local_psql -c "select string_agg(class_id || ':' || n, ' ')
      from (select class_id, count(*) n from seat_enr group by class_id
            order by class_id) t")" = '1:3 2:3 3:3' ] ||
    fail "seat SSI: $kind, a class does not hold 3 rows"
  local_psql -c 'drop table seat_enr, seat_class' >> "$work/seat-$kind.log" 2>&1
done
