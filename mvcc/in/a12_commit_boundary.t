# a12 -- the commit boundary decides what a snapshot sees.
#
# T2 begins before T1 commits and must see nothing of it, ever.
# T3 begins afterwards and sees all of it. Visibility is by
# commit order against each snapshot, not by who asked first.

pairings all
session T1
session T2
session T3 rr
seed R1 0

@1 T1 begin
@2 T1 write R1 t1
@2 T1 insert R2 t1
@3 T2 begin
@4 T2 read R1
@4 check last T2 0
@4 T2 read R2
@4 check last T2 -
@5 T1 commit
@6 T2 read R1
@6 check last T2 0
@6 T2 read R2
@6 check last T2 -
@7 T2 commit
@8 T3 begin
@9 T3 read R1
@9 check last T3 t1
@9 T3 read R2
@9 check last T3 t1
@10 T3 commit
@11 check committed T1 T2 T3
@11 check row R1 t1
@11 check row R2 t1
