# k08 -- a writer that kept the key, then changes it, waits for the
# key-share lock that passed over its first version.
#
# T1 updates R1, keeping its key. T2's KS lock passes over T1's
# version in flight. T1 then writes R1 again and changes the key:
# that write is X, meets T2's KS lock, and waits until T2 commits.

stores chained
pairings rr
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 a keykept
@3 T2 lock R1 keyshare
@4 check last T2 ok
@5 T1 write R1 b blocks
@6 T2 commit
@7 check last T1 ok
@8 T1 commit
@9 check committed T1 T2
@10 check row R1 b
