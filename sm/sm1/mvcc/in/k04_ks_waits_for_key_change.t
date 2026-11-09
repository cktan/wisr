# k04 -- a key-share lock waits for an update in flight that changed
# the key, then fails its currency check.
#
# T1's update changes R1's key, unflagged. T2's KS lock waits for
# T1. When T1 commits, the newest version of R1 changed the key
# after T2's snapshot, so T2's lock fails with conflict, as
# PostgreSQL's FOR KEY SHARE fails on HEAP_KEYS_UPDATED.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T2 read R1
@3 T1 write R1 t1
@4 T2 lock R1 keyshare blocks
@5 T1 commit
@6 check last T2 conflict
@7 T2 abort
@8 check committed T1
@9 check row R1 t1
