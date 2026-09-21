# f08 -- READ COMMITTED: three sessions in turn on one hot row.
#
# All three begin before any commit. T2 queues behind T1; T3
# queues behind T2 once T2 holds the lock. Each in turn wakes
# with `updated`, reads the newest version, writes, and
# commits, so the counter ends at the last writer's value.

pairings rr
session T1 rc
session T2 rc
session T3 rc
seed R1 0

@1 T1 begin
@1 T2 begin
@1 T3 begin
@2 T1 write R1 1
@3 T2 write R1 2 blocks
@4 T1 commit
@5 check last T2 updated
@6 T3 write R1 3 blocks
@7 T2 readlatest R1
@7 check last T2 1
@8 T2 write R1 2
@9 T2 commit
@10 check last T3 updated
@11 T3 readlatest R1
@11 check last T3 2
@12 T3 write R1 3
@13 T3 commit
@14 check committed T1 T2 T3
@14 check row R1 3
