# c07 -- Fekete's read-only anomaly.
#
# Savings R1 and checking R2 both hold 0, and a withdrawal from
# an overdrawn total costs 1 more. T2 reads both and decides on
# 12; T1's deposit of 20 commits; T3 reads and sees 20 and 0 --
# a state no serial order produces once T2 commits its
# penalty-priced withdrawal. T3 only reads, and is still a full
# participant: at S+S not all three may commit.

pairings all
session T1
session T2
session T3
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
@12 check row R2 -12 when rr
