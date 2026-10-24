# g02 -- an aborted writer's versions, passed, then written over.
#
# T2 writes R1 twice and aborts: its versions stay newest, dead.
# T1, begun before, and T3, begun after, read past both to 0. T3
# writes R1 over them and commits; T1 still reads 0, and a later
# reader reads T3's value. T3's insert of R2, which exists, fails;
# its write of R9, which does not, fails too.

pairings all
session T1
session T2
session T3 rr
seed R1 0
seed R2 0

@1 T1 begin
@2 T1 read R1
@3 T2 begin
@4 T2 write R1 1
@5 T2 write R1 2
@6 T2 abort
@7 T3 begin
@8 T3 read R1
@9 T1 read R1
@10 T3 insert R2 x
@11 T3 abort
@12 T3 begin
@13 T3 write R9 x
@14 T3 abort
@15 T3 begin
@16 T3 write R1 3
@17 T3 commit
@18 T1 read R1
@19 T1 scan R1 R2
@20 T1 commit
@21 check row R1 3
@21 check rows
