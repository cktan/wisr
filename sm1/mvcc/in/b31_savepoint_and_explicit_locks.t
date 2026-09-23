# b31 -- what a rollback does to locks sm1_lock took.
#
# b25 makes the point for locks a write took; these are taken
# outright. R2 was locked after the mark, so the rollback
# gives it back. R1 was locked before the mark and then
# upgraded after it, and the upgrade is NOT undone: a
# transaction that has held a row exclusive keeps it that way
# to the end. T2's shared lock on R1 is what tells the two
# apart -- it would be granted against a shared holder and is
# not.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@2 T1 lock R1 shared
@3 T1 savepoint
@4 T1 lock R2 shared
@5 T1 write R1 t1
@6 T1 rollback 1
@7 T2 begin
@8 T2 lock R2 shared
@9 check last T2 ok
@10 T2 lock R1 shared blocks
@11 T1 abort
@12 check last T2 ok
@13 T2 commit
@14 check row R1 0
@14 check row R2 0
