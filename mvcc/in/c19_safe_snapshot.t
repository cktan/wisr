# c19 -- reads alone never doom a transaction.
#
# T1 has no incoming edge, so however long it lives and
# whatever it writes afterwards it commits -- here beside a
# concurrent writer, and then again with nothing else running.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@2 T1 read R1
@3 T2 begin
@4 T2 write R2 t2
@5 T2 commit
@6 T1 write R1 t1
@7 T1 commit
@8 T1 begin
@9 T1 read R1
@9 T1 scan R1 R2
@10 T1 write R1 again
@11 T1 commit
@12 check committed T1 T2
@13 check row R1 again
