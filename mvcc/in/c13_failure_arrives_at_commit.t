# c13 -- where a doomed transaction finds out.
#
# The c02 shape, asking a different question: every read and
# write returns ok, doomed or not, and the failure appears at
# the commit. The retry at the end -- each session writing its
# row again, with nobody concurrent -- must then succeed, which
# is what makes SM_ERR_SERIALIZATION a retryable error.

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
@8 check at-most-one-committed T1 T2 when v+v
@9 T1 begin
@10 T1 write R1 t1
@11 T1 commit
@12 T2 begin
@13 T2 write R2 t2
@14 T2 commit
@15 check row R1 t1
@15 check row R2 t2
