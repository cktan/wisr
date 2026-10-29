# h01 -- a reader's table lock in a slot still stops DDL.
#
# A table's IS, as a reader takes it, is in a slot of the
# reader's session, not in the lock table (SM1_DESIGN.md §5.1).
# DDL's X must still wait for it: X counts itself, then moves
# every session's slot for the oid into the table, where it
# meets T1's IS and waits. T1's commit lets it through.

pairings rr
session T1
session T2

@1 T1 begin
@1 T2 begin
@2 T1 lockoid 1 is
@3 T2 lockoid 1 exclusive blocks
@4 T1 commit
@5 check last T2 ok
@6 T2 commit
@7 check committed T1 T2
