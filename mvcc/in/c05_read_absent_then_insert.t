# c05 -- reading a row that is not there counts as a read.
#
# Each transaction inserts exactly the row the other looked for
# and did not find, so the edges run both ways even though
# neither read returned anything.

pairings all
session T1
session T2

@1 T1 begin
@1 T2 begin
@2 T1 read R2
@3 T2 read R1
@4 T1 insert R1 t1
@5 T2 insert R2 t2
@6 T1 commit?
@7 T2 commit?
@8 check committed T1 T2 when rr
@8 check committed T1 T2 when rr+s
@8 check at-most-one-committed T1 T2 when s+s
