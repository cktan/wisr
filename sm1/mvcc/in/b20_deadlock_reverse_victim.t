# b20 -- the deadlock victim is one that was already waiting.
#
# b16 dooms the transaction whose own step closes the cycle,
# which is the easy case: it fails where it stands. Here the
# cycle is closed by the older transaction, so the victim is
# the younger one, and it is already blocked on a lock.
# Nothing is running on its behalf to return an error, so the
# engine must reach the waiter and wake it. T1's step
# succeeds, and T2 resumes with deadlock.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 t1
@3 T2 write R2 t2
@4 T2 write R1 t2 blocks
@5 T1 write R2 t1
@6 check last T1 ok
@7 check last T2 deadlock
@8 T1 commit
@9 check committed T1
@10 check row R1 t1
@10 check row R2 t1
