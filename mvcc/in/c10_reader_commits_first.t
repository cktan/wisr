# c10 -- the reader finishes before the writer starts.
#
# The shape of c02 with the concurrency removed: T2 begins only
# after T1 committed, so the history is the serial one. This
# must never fail.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@2 T1 read R1
@3 T1 write R2 t1
@4 T1 commit
@5 T2 begin
@6 T2 read R2
@7 T2 write R1 t2
@8 T2 commit
@9 check committed T1 T2
@10 check rows
