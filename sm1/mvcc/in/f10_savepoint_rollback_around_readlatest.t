# f10 -- READ COMMITTED: a savepoint rollback and the re-read.
#
# T1 commits R1 and R2 after T2 began.
#
# R1: T2's lock is taken after the savepoint, by the write that
# fails with `updated`. readlatest sets the mark on it, then the
# rollback releases the lock and the mark with it, so the write
# is refused again until T2 reads again.
#
# R2: T2 locks it before the savepoint and reads it after. The
# rollback keeps the lock, so it keeps the mark, and the write
# goes ahead.

pairings rr
session T1 rc
session T2 rc
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 1
@2 T1 write R2 1
@3 T1 commit
@4 T2 lock R2 exclusive
@4 check last T2 ok
@5 T2 savepoint
@6 T2 write R1 x
@6 check last T2 updated
@7 T2 readlatest R1
@7 check last T2 1
@7 T2 readlatest R2
@7 check last T2 1
@8 T2 rollback 1
@9 T2 write R1 x
@9 check last T2 updated
@10 T2 write R2 y
@10 check last T2 ok
@11 T2 readlatest R1
@12 T2 write R1 x
@12 check last T2 ok
@13 T2 commit
@14 check committed T1 T2
@14 check row R1 x
@14 check row R2 y
