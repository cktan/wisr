# h02 -- while DDL holds a table, a reader takes no slot.
#
# X on an oid is counted, so a reader's IS on it goes to the
# lock table instead of a slot, and waits there for T1. Once T1
# commits, the count is back to 0, and T3's IS, asked after,
# takes a slot and is granted at once.

pairings rr
session T1
session T2
session T3

@1 T1 begin
@1 T2 begin
@1 T3 begin
@2 T1 lockoid 1 exclusive
@3 T2 lockoid 1 is blocks
@4 T1 commit
@5 check last T2 ok
@6 T3 lockoid 1 is
@7 T2 commit
@7 T3 commit
@8 check committed T1 T2 T3
