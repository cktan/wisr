# f03 -- READ COMMITTED: the row was deleted while T2 waited.
#
# T1 deletes R1 and commits. T2's write is refused with
# `updated`; readlatest reports the row absent, so T2 skips the
# update.

pairings rr
session T1 rc
session T2 rc
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 delete R1
@3 T2 write R1 2 blocks
@4 T1 commit
@5 check last T2 updated
@6 T2 readlatest R1
@6 check last T2 -
@7 T2 commit
@8 check committed T1 T2
@8 check row R1 -
