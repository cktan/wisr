# b12 -- inserting a row that existed before the snapshot.
#
# Nothing changed after T1's snapshot, so this is not a
# conflict and not retryable: it is the ordinary "the key is
# taken". The transaction stays active and carries on.

pairings all
session T1
seed R1 0

@1 T1 begin
@2 T1 insert R1 t1
@3 check last T1 dupkey
@4 T1 write R1 t1
@5 T1 commit
@6 check row R1 t1
