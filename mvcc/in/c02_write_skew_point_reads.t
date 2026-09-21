# c02 -- write skew over two rows read by point read.
#
# Each transaction writes a row the other read, so
# T1 --rw--> T2 and T2 --rw--> T1: a cycle. At S+S they cannot
# both commit; at RR, and in a mixed pair where only one side
# is tracked, both do. The commits are written "commit?"
# because at S+S either side may be the one doomed.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T1 read R1
@2 T1 read R2
@3 T2 read R1
@3 T2 read R2
@4 T1 write R1 t1
@5 T2 write R2 t2
@6 T1 commit?
@7 T2 commit?
@8 check committed T1 T2 when rr
@8 check committed T1 T2 when rr+s
@8 check at-most-one-committed T1 T2 when s+s
