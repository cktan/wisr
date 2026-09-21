# a03 -- an aborted transaction leaves nothing behind.
#
# T1 writes, deletes, and inserts, then aborts. T2 must see the
# original rows and must be free to write the same rows: an
# aborted write leaves no version and no lock.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@2 T1 write R1 t1
@2 T1 delete R2
@2 T1 insert R3 t1
@3 T1 abort
@4 T2 begin
@5 T2 read R1
@5 T2 read R2
@5 T2 read R3
@6 T2 write R1 t2
@6 T2 delete R2
@6 T2 insert R3 t2
@7 T2 commit
@8 check row R1 t2
@8 check row R2 -
@8 check row R3 t2
@8 check rows
