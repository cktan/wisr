# k12 -- an update that keeps the key over the transaction's own
# insert does not let a key-share lock pass the insert.
#
# T1 inserts R1, then updates it, keeping its key. The update is not
# flagged, as the insert it replaces is not: T2's KS lock waits for
# T1, as it would for the insert alone, and is taken once T1 commits.
# T2 is at READ COMMITTED, so the new row is no conflict.

stores chained
pairings rr
session T1
session T2 rc
seed R0 0

@1 T1 begin
@1 T2 begin
@2 T1 insert R1 new
@3 T1 write R1 new_again keykept
@4 T2 lock R1 keyshare blocks
@5 T1 commit
@6 check last T2 ok
@7 T2 commit
@8 check committed T1 T2
@9 check row R1 new_again
