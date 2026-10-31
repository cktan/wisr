# h08 -- a table lock's upgrade past a savepoint is not undone.
#
# T1 takes table 1's IX, in a slot, sets a savepoint, and
# upgrades to X: the slot goes to the lock table and converts
# there, adding no entry. Rolling back to the savepoint leaves
# the X held, as b31 says of a row lock. T2's IS on the table
# is what tells IX and X apart: granted against IX, it waits
# for T1's commit.

pairings rr
session T1
session T2

@1 T1 begin
@1 T2 begin
@2 T1 lockoid 1 ix
@3 T1 savepoint
@4 T1 lockoid 1 exclusive
@5 T1 rollback 1
@6 T2 lockoid 1 is blocks
@7 T1 commit
@8 check last T2 ok
@9 T2 commit
@10 check committed T1 T2
