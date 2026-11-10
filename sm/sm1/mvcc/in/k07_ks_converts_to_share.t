# k07 -- a key-share holder that asks for share converts to S, which
# an update that keeps the key waits for.
#
# T1 locks R1 in KS, then in S: it holds S, the weaker mode that
# covers both. T2's update keeps the key, so it writes in NKU, which
# S refuses: T2 waits until T1 commits. On a versioned store the
# update is X, and waits as well.

pairings rr
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 lock R1 keyshare
@3 T1 lock R1 shared
@4 T2 write R1 t2 keykept blocks
@5 T1 commit
@6 check last T2 ok
@7 T2 commit
@8 check committed T1 T2
@9 check row R1 t2
