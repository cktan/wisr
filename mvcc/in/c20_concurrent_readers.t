# c20 -- readers do not block or conflict with each other.
#
# Reads take no locks, and an SIREAD is not a lock: it never
# blocks and never fails a reader. Afterwards one of them
# writes, to show the reads left nothing behind.

pairings all
session T1
session T2
session T3
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@1 T3 begin
@2 T1 read R1
@2 T1 scan R1 R2
@3 T2 read R1
@3 T2 scan R1 R2
@4 T3 read R1
@4 T3 scan R1 R2
@5 T1 commit
@5 T2 commit
@5 T3 commit
@6 T1 begin
@7 T1 write R1 t1
@8 T1 commit
@9 check committed T1 T2 T3
