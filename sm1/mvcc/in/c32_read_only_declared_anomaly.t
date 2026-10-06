# c32 -- Fekete's read-only anomaly, with T3 declared read-only.
#
# c07 with T3 begun read-only. T3 begins after T1's commit, so
# T1, the T3 of the dangerous structure T3 --rw--> T2 --rw--> T1,
# committed before T3's snapshot: the declaration excuses
# nothing, and T3's snapshot is never safe while T2 runs. At S+S
# not all three may commit, as in c07.

pairings all
session T1
session T2
session T3 ro
seed R1 0
seed R2 0

@1 T2 begin
@2 T2 read R1
@2 T2 read R2
@3 T1 begin
@4 T1 write R1 20
@5 T1 commit
@6 T3 begin
@7 T3 read R1
@7 T3 read R2
@8 T3 commit?
@9 T2 write R2 -12
@10 T2 commit?
@11 check committed T1 T2 T3 when rr
@11 check committed T1 T2 T3 when rr+s
@11 check not-all-committed T1 T2 T3 when s+s
