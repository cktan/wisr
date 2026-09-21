# a02 -- dirty read through a scan, for an insert and a delete.
#
# T1 inserts R3 and deletes R2 without committing. T2's scan
# must show neither, and must not change when T1 commits.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T1 insert R3 t1
@2 T1 delete R2
@3 T2 scan R1 R3
@4 T1 commit
@5 T2 scan R1 R3
@6 T2 commit
@7 check row R1 0
@7 check row R2 -
@7 check row R3 t1
@7 check rows
