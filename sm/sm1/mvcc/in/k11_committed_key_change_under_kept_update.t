# k11 -- KS's currency check fails on a committed key change that a
# later update of the same transaction kept.
#
# T1 changes R1's key, updates R1 again keeping that key, and commits.
# T2 read R1 before, so its KS lock checks every version committed
# since its snapshot: the key change is among them, and the lock
# fails with conflict, whatever the newest version's flag says.

stores chained
pairings rr
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T2 read R1
@3 T1 write R1 changed
@4 T1 write R1 changed_again keykept
@5 T1 commit
@6 T2 lock R1 keyshare
@7 check last T2 conflict
@8 check committed T1
@9 check row R1 changed_again
