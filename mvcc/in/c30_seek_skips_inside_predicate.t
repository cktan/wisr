# c30 -- the keys a seek jumps over are still read.
#
# c11 has a cursor stop early, and the narrowing lets a write
# past the last key it examined through. A seek is the other
# shape: T1 reaches R8, so R8 is the last key examined, and
# everything below it is inside the predicate whether the
# cursor looked at it or not. T2 writes R5, which T1 skipped
# entirely, and the edge forms all the same. That is an
# over-approximation and it is the safe direction -- a
# predicate that tracked only the keys actually returned would
# miss this write, and with T2 --rw--> T1 the cycle would go
# undetected.

pairings all
session T1
session T2
seed R1 1
seed R2 2
seed R5 5
seed R8 8

@1 T1 begin
@1 T2 begin
@2 T1 open C R1 R9
@3 T1 next C
@4 T1 seek C R8
@5 T1 next C
@6 T1 close C
@7 T2 write R5 t2
@8 T2 read R2
@9 T1 write R2 t1
@10 T1 commit?
@10 T2 commit?
@11 check committed T1 T2 when rr
@11 check committed T1 T2 when rr+s
@11 check at-most-one-committed T1 T2 when s+s
