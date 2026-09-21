# b13 -- updating or deleting a row that never existed.
#
# Not retryable, and the transaction stays active. Compare b03
# and b05, where the row existed in the snapshot and was
# removed by someone else: those are conflicts.

pairings all
session T1
seed R1 0

@1 T1 begin
@2 T1 write R2 t1
@3 check last T1 notfound
@4 T1 delete R2
@5 check last T1 notfound
@6 T1 insert R2 t1
@7 T1 commit
@8 check row R2 t1
