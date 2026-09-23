# b26 -- two shared locks on one row, and a writer behind
# them.
#
# The first mode of sm1_lock. A shared lock is not a write, so
# two transactions may hold one row at once and neither waits;
# what it excludes is writing. T3's write therefore queues
# behind both, and one holder leaving is not enough -- the
# lock is free only when the last of them ends. Nothing was
# written, so the waiter finds the row as its snapshot has it
# and succeeds.

pairings all
session T1
session T2
session T3
seed R1 0

@1 T1 begin
@1 T2 begin
@1 T3 begin
@2 T1 lock R1 shared
@3 T2 lock R1 shared
@4 T3 write R1 t3 blocks
@5 T1 commit
@6 T2 commit
@7 check last T3 ok
@8 T3 commit
@9 check committed T1 T2 T3
@10 check row R1 t3
