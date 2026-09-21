# c23 -- a scan that steps past a newer version.
#
# c16 with a range scan in place of the point read. T2's write
# of R1 commits before T1's scan even opens its range, so no
# edge can come from the range: the only way for T1 to learn
# of T2 is by stepping past its version mid-scan, which is
# T1 --rw--> T2. T1's write of R2, which T2 read, closes the
# structure onto a committed pivot, and T1 is the one doomed.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T2 read R2
@3 T2 write R1 t2
@4 T2 commit
@5 T1 scan R1 R3
@6 T1 write R2 t1
@7 T1 commit
@8 check committed T1 when rr
@8 check committed T1 when rr+s
@8 check failed T1 when s+s
@9 check row R2 t1 when rr
@9 check row R2 0 when s+s
