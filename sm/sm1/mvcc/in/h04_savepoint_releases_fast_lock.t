# h04 -- a rollback to a savepoint gives back a slot.
#
# T1 takes the table's IS after its savepoint, in a slot, and
# rolls back to the savepoint: the lock goes with what was
# undone, so T2's X on the table is granted at once, while T1's
# IS on oid 2, taken before the savepoint, still stops T2's X
# on oid 2.

pairings rr
session T1
session T2

@1 T1 begin
@1 T2 begin
@2 T1 lockoid 2 is
@3 T1 savepoint
@4 T1 lockoid 1 is
@5 T1 rollback 1
@6 T2 lockoid 1 exclusive
@7 T2 lockoid 2 exclusive blocks
@8 T1 commit
@9 check last T2 ok
@10 T2 commit
@11 check committed T1 T2
