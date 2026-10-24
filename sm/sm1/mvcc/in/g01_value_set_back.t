# g01 -- a row set to a value, another, then the first again.
#
# T2 sets R1 from 0 to 1, commits, then back to 0, commits. T1's
# snapshot, taken first, reads the first 0; T3's, taken between,
# reads 1 even after R1 holds 0 again: each reader finds the
# version of its own snapshot, not the newest one with the value
# it saw. A scan agrees with the point reads. T3's write of R1
# then fails: R1 changed after its snapshot.

pairings rr
session T1
session T2
session T3 rr
seed R1 0
seed R2 0

@1 T1 begin
@2 T1 read R1
@3 T2 begin
@4 T2 write R1 1
@5 T2 commit
@6 T3 begin
@7 T3 read R1
@8 T2 begin
@9 T2 write R1 0
@10 T2 commit
@11 T1 read R1
@12 T1 scan R1 R2
@13 T3 read R1
@14 T3 write R1 3
@15 T3 abort
@16 T1 commit
@17 check row R1 0
@17 check rows
