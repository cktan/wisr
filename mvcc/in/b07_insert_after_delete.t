# b07 -- an insert over a committed delete.
#
# T1 would be inserting a row its own snapshot says exists, and
# the row changed after that snapshot. The conflict is reported
# before the "already exists" rule is reached.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@2 T1 read R1
@3 T2 begin
@4 T2 delete R1
@5 T2 commit
@6 T1 insert R1 t1
@7 check last T1 conflict
@8 check row R1 -
