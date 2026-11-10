# g03 -- the row reaper cuts a chain only when no snapshot needs it.
#
# T1 reads R1 at its snapshot, holding the watermark back. T2 updates
# R1 twice and commits. T3's read of R1 queues the key for the row
# reaper, whose pass, run by T2 with no transaction, reaps nothing:
# CURR is not settled, and T1 still reads 0 through the chain. Once T1
# ends, a read queues R1 again, and a pass reaps it; every reader then
# sees T2's last value (../../GC.md §13.5).

stores chained
pairings rr
session T1
session T2
session T3 rc
seed R1 0

@1 T1 begin
@2 T1 read R1
@3 T2 begin
@3 T2 write R1 a
@3 T2 commit
@4 T2 begin
@4 T2 write R1 b
@4 T2 commit
@5 T3 begin
@5 T3 read R1
@5 T3 commit
@6 T2 reap
@7 check last T2 0
@8 T1 read R1
@9 check last T1 0
@10 T1 commit
@11 T3 begin
@11 T3 read R1
@11 T3 commit
@12 T2 reap
@13 check last T2 1
@14 T3 begin
@14 T3 read R1
@14 T3 commit
@15 T2 reap
@16 check last T2 0
@17 check row R1 b
