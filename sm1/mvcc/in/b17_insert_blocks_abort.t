# b17 -- two inserts of the same absent row, the first
# aborting.
#
# The other half of b11: an insert takes the row lock on a key
# that is not there, so the second inserter waits, and an
# aborted insert is no insert at all. The waiter therefore
# finds the key still absent and succeeds, as the waiting
# update of b10 does.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 insert R2 t1
@3 T2 insert R2 t2 blocks
@4 T1 abort
@5 check last T2 ok
@6 T2 commit
@7 check committed T2
@8 check row R2 t2
