# f02 -- READ COMMITTED: the re-check fails, so nothing is written.
#
# The statement is UPDATE R1 SET n = 1 WHERE n = 0: claim the
# row once. T1 claims it first. T2's write is refused with
# `updated`; readlatest shows R1 is now 1, so T2's WHERE clause
# no longer holds and T2 writes nothing. Writing blindly
# against the newest version would have changed a row that no
# longer qualifies.

pairings rr
session T1 rc
session T2 rc
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 1
@3 T2 write R1 1 blocks
@4 T1 commit
@5 check last T2 updated
@6 T2 readlatest R1
@6 check last T2 1
@7 T2 commit
@8 check committed T1 T2
@8 check row R1 1
