# b10 -- a write waits for the row lock, then proceeds.
#
# The other half of b09: an aborted write is no write at all,
# so the waiter has nothing to lose to and its write succeeds.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 t1
@3 T2 write R1 t2 blocks
@4 T1 abort
@5 check last T2 ok
@6 T2 commit
@7 check row R1 t2
