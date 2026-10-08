# a13 -- a snapshot survives a series of commits.
#
# T1 scans once at the start; after each of T2's committed
# transactions the same scan must give the same answer. A
# snapshot is a fixed view, not a moving one.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@2 T1 scan R1 R5
@3 T2 begin
@4 T2 insert R3 t2
@4 T2 write R2 1
@5 T2 commit
@6 T1 scan R1 R5
@7 T2 begin
@8 T2 insert R4 t2
@8 T2 write R2 2
@9 T2 commit
@10 T1 scan R1 R5
@11 T2 begin
@12 T2 delete R1
@12 T2 write R2 3
@13 T2 commit
@14 T1 scan R1 R5
@15 T1 commit
@16 check row R1 -
@16 check row R2 3
@16 check row R3 t2
@16 check row R4 t2
@16 check rows
