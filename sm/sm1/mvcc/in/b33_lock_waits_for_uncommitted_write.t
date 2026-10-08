# b33 -- an explicit lock waits for an uncommitted write.
#
# A write keeps no lock entry: its uncommitted version stands
# for the lock (WRITELOCK.md). So a lock on a row another
# transaction has written and not committed must wait for that
# transaction, as FOR SHARE waits for an uncommitted update in
# PostgreSQL. T1 updates R1; T2 asks for R1 shared and waits.
# When T1 commits, T2's lock is granted and its check finds R1
# committed after its snapshot: a conflict. R2, which T1 did
# not write, is locked at once.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 t1
@3 T2 lock R2 shared
@4 check last T2 ok
@5 T2 lock R1 shared blocks
@6 T1 commit
@7 check last T2 conflict
@8 check committed T1
@9 check row R1 t1
