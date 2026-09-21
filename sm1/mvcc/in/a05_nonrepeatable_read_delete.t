# a05 -- read-write-read with a delete.
#
# A row deleted after T1's snapshot stays visible to T1,
# through both the point read and the scan.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@2 T1 read R1
@3 T2 begin
@4 T2 delete R1
@5 T2 commit
@6 T1 read R1
@6 T1 scan R1 R2
@7 T1 commit
@8 check row R1 -
