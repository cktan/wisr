# b16 -- two writers that wait on each other.
#
# T1 holds R1 and waits for R2; T2 holds R2 and asks for R1,
# closing the cycle. Waiting is not an answer here: nobody
# will ever release, so the engine must break the cycle
# itself. It picks the youngest transaction, T2, which fails
# with deadlock and drops its locks, and T1's wait ends.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 t1
@3 T2 write R2 t2
@4 T1 write R2 t1 blocks
@5 T2 write R1 t2
@6 check last T2 deadlock
@7 T1 commit
@8 check committed T1
@9 check row R1 t1
@10 check row R2 t1
