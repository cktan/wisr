# b25 -- a rollback gives back the locks taken after the mark.
#
# T1 locks R1, marks, then locks R2 by writing it, and T2
# queues on R2. Rolling back to the mark undoes that write, so
# the lock has nothing left to protect and T2 goes through at
# once -- no one ever saw T1's intent on R2. R1 was locked
# before the mark and stays locked: the rollback is to a
# point, not a release of everything.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 t1
@3 T1 savepoint
@4 T1 write R2 t1
@5 T2 write R2 t2 blocks
@6 T1 rollback 1
@7 check last T2 ok
@8 T2 write R1 t2 blocks
@9 T1 commit
@10 check last T2 conflict
@11 check committed T1
@12 check row R1 t1
@12 check row R2 0
