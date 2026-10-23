# f11 -- READ COMMITTED: the row moved to another key while T2
# waited.
#
# T1 erases R1 as a move does, its primary key changed, and
# commits. T2's write is refused with `updated`, as over any
# newer commit; readlatest then says `moved`, not absent, and so
# does the write taken again: the row lives on elsewhere, and T2
# must not take it for deleted (ROWKEY.md §2.3). A reader sees
# the move's tombstone as any other.

pairings rr
session T1 rc
session T2 rc
session T3 rc
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 move R1
@3 T2 write R1 2 blocks
@4 T1 commit
@5 check last T2 updated
@6 T2 readlatest R1
@6 check last T2 moved
@7 T2 write R1 2
@7 check last T2 moved
@8 T2 abort
@9 T3 begin
@10 T3 read R1
@10 check last T3 -
@11 T3 delete R1
@11 check last T3 moved
@12 T3 commit
@13 check committed T1 T3
@13 check row R1 -
