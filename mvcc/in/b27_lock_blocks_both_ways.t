# b27 -- shared against exclusive, in both directions.
#
# A shared lock stops a write, and an exclusive lock stops a
# shared request: the mode is a property of the lock, not of
# who asks. T1 holds R1 shared and T2's write waits. T3 holds
# R2 exclusive and T1's shared request waits. Each waiter is
# released by its holder ending, and the two chains are
# independent -- T1 is a holder and a waiter at once.

pairings all
session T1
session T2
session T3
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@1 T3 begin
@2 T1 lock R1 shared
@3 T2 write R1 t2 blocks
@4 T3 lock R2 exclusive
@5 T1 lock R2 shared blocks
@6 T3 abort
@7 check last T1 ok
@8 T1 abort
@9 check last T2 ok
@10 T2 commit
@11 check committed T2
@12 check row R1 t2
@12 check row R2 0
