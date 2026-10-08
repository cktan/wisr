# f09 -- READ COMMITTED: a delete meets a concurrent update.
#
# The statement is DELETE FROM R WHERE n = 0. T1 sets R1 to 1
# and commits before T2's delete gets there. T2's delete is
# refused with `updated`; readlatest shows n = 1, the predicate
# no longer holds, and T2 skips the delete.

pairings rr
session T1 rc
session T2 rc
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 1
@3 T1 commit
@4 T2 delete R1
@4 check last T2 updated
@5 T2 readlatest R1
@5 check last T2 1
@6 T2 commit
@7 check committed T1 T2
@7 check row R1 1
