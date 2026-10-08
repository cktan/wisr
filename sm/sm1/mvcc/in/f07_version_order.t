# f07 -- READ COMMITTED: a writer that began before a commit
# writes past it, and the later commit wins.
#
# T1 writes R2 first, so its transaction id is old. T2 commits R1 = 1
# afterwards. T1's write of R1 is refused with `updated`; after
# readlatest it goes ahead and commits R1 = 2 last. Versions
# must sort in commit order, not in transaction id order: a
# reader that begins afterwards must see 2. (test_gc.cc adds
# compaction, which the driver cannot force: the newer commit
# must not be dropped as superseded.)

pairings rr
session T1 rc
session T2 rc
session T3 rr
seed R1 0
seed R2 0

@1 T1 begin
@2 T1 write R2 1
@3 T2 begin
@4 T2 write R1 1
@5 T2 commit
@6 T1 write R1 2
@6 check last T1 updated
@7 T1 readlatest R1
@7 check last T1 1
@8 T1 write R1 2
@9 T1 commit
@10 T3 begin
@11 T3 read R1
@11 check last T3 2
@12 T3 commit
@13 check committed T1 T2 T3
