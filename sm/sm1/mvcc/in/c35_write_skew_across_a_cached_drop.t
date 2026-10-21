# c35 -- c31 with T2's lookup of the store already cached.
#
# SM1_DESIGN.md §11's store cache. T2's session looks the
# second store up in a transaction of its own first, so its
# store cache holds the store as live, at a snapshot no DDL has
# passed. c31's schedule then runs: were T2's lookup answered
# from that entry, it would take no SIREAD on the store's
# catalog row, the edge T2 --rw--> T1 would not form, and under
# S+S both would commit. A serializable lookup trusts only an
# entry its own transaction read, so it reads the catalog and
# the cycle closes as in c31.

pairings all
session T1
session T2
seed R1 0

@1 T2 begin
@1 T2 lookup R1
@1 T2 commit
@2 T1 begin
@2 T2 begin
@3 T1 read R1
@4 T2 lookup R1
@5 T1 rmstore
@6 T2 write R1 t2
@7 T1 commit?
@7 T2 commit?
@8 check committed T1 T2 when rr
@8 check committed T1 T2 when rr+s
@8 check at-most-one-committed T1 T2 when s+s
