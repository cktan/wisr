# b05 -- a delete over a committed delete.
#
# Two transactions deleting one row is a write-write conflict.
# The loser must be told to retry, not told the row was never
# there.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@2 T1 read R1
@3 T2 begin
@4 T2 delete R1
@5 T2 commit
@6 T1 delete R1
@7 check last T1 conflict
