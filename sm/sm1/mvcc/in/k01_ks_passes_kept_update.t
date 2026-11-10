# k01 -- a key-share lock passes over an update in flight that kept
# its key.
#
# T1 updates R1's non-key columns, as UPDATE customer SET phone = ...
# does, and has not committed. T2 checks a foreign key to R1 and
# locks it in KS. The version T1 wrote is flagged key-kept, so T2
# does not wait for T1: both commit (../../LOCKMGR.md §1.13).

stores chained
pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 t1 keykept
@3 T2 lock R1 keyshare
@4 check last T2 ok
@5 T1 commit
@6 T2 commit
@7 check committed T1 T2
@8 check row R1 t1
