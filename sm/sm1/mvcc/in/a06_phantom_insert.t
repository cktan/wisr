# a06 -- no phantom from an insert.
#
# R2 is inserted after T1's snapshot, so no repeated read of
# the range may show it. Whether T1 could then WRITE on the
# strength of its scan is c03, not this.

pairings all
session T1
session T2
seed R1 0
seed R3 0

@1 T1 begin
@2 T1 scan R1 R3
@3 T2 begin
@4 T2 insert R2 t2
@5 T2 commit
@6 T1 scan R1 R3
@6 T1 scan R2 R2
@6 T1 read R2
@7 T1 commit
@8 check row R1 0
@8 check row R2 t2
@8 check row R3 0
@8 check rows
