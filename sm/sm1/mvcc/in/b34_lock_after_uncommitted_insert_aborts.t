# b34 -- an explicit lock waits for an uncommitted insert,
# which aborts.
#
# The other half of b33. T1 inserts R3, absent until then, and
# T2 asks for R3 exclusive, as SELECT ... FOR UPDATE would: it
# waits for T1. T1 aborts, so its version is dead and there was
# never a row; T2's lock is granted, and its own insert of R3
# then goes through.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 insert R3 t1
@3 T2 lock R3 exclusive blocks
@4 T1 abort
@5 check last T2 ok
@6 T2 insert R3 t2
@7 check last T2 ok
@8 T2 commit
@9 check row R3 t2
