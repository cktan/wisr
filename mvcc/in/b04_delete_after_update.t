# b04 -- a delete over a committed update.
#
# A delete is a write like any other and loses to the earlier
# committer.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@2 T1 read R1
@3 T2 begin
@4 T2 write R1 t2
@5 T2 commit
@6 T1 delete R1
@7 check last T1 conflict
@8 check row R1 t2
