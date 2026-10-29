# h05 -- a deadlock through a lock that was in a slot.
#
# T1 reads table 1, its IS in a slot; T2 alters table 2; T1
# asks X on table 2 and waits for T2. T2 then asks X on table
# 1: its sweep moves T1's IS into the lock table, T2 waits for
# T1, and the cycle is found. The younger, T2, is the victim,
# and T1's X on table 2 is granted.

pairings rr
session T1
session T2

@1 T1 begin
@1 T2 begin
@2 T1 lockoid 1 is
@3 T2 lockoid 2 exclusive
@4 T1 lockoid 2 exclusive blocks
@5 T2 lockoid 1 exclusive
@6 check last T2 deadlock
@7 check last T1 ok
@8 T1 commit
@9 check committed T1
