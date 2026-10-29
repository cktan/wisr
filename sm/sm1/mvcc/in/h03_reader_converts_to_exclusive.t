# h03 -- a reader's own slot converts to DDL's lock.
#
# T1 reads the table, its IS in a slot, then alters it: its X
# sweeps T1's own slot into the lock table and converts it. T2's
# IS then waits for T1, as for any X, until T1 commits.

pairings rr
session T1
session T2

@1 T1 begin
@1 T2 begin
@2 T1 lockoid 1 is
@3 T1 lockoid 1 exclusive
@4 T2 lockoid 1 is blocks
@5 T1 commit
@6 check last T2 ok
@7 T2 commit
@8 check committed T1 T2
