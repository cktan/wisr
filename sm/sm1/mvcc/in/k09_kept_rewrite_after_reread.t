# k09 -- at READ COMMITTED, an update that keeps its key is refused
# for a newer commit, re-reads, and writes.
#
# T1 updates R1, keeping its key, and commits after T2 began. T2's
# update, keeping the key too, is refused with updated, and holds R1
# in NKU, its write's mode. T2 reads the newest version under that
# lock, which keeps every other writer out, and its repeated write
# goes through, as one under X does (../../LOCKMGR.md §1.9).

pairings levels
session T1
session T2 rc
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 t1 keykept
@3 T1 commit
@4 T2 write R1 t2 keykept
@5 check last T2 updated
@6 T2 readlatest R1
@7 T2 write R1 t2 keykept
@8 check last T2 ok
@9 T2 commit
@10 check committed T1 T2
@11 check row R1 t2
