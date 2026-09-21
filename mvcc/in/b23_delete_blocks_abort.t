# b23 -- a delete waiting on a live writer that aborts.
#
# b22 with the holder aborting. An aborted delete is no
# delete, so the waiter finds the row exactly as its snapshot
# has it and succeeds, the way b10's waiting update and b17's
# waiting insert do. The row is gone only because the second
# delete removed it.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 delete R1
@3 T2 write R1 t2 blocks
@4 T1 abort
@5 check last T2 ok
@6 T2 delete R1
@7 T2 commit
@8 check committed T2
@9 check row R1 -
