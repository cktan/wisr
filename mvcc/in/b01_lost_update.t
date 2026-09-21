# b01 -- read-write-write: the lost update.
#
# T1 reads R1 and means to write it back. T2 gets there first.
# T1's write must fail with a conflict rather than overwrite a
# value it never saw. The conflict is retryable and aborts T1.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@2 T1 read R1
@3 T2 begin
@4 T2 write R1 t2
@5 T2 commit
@6 T1 write R1 t1
@7 check last T1 conflict
@8 check row R1 t2
