# a16 -- a statement boundary pins read committed's snapshot.
#
# a15's fresh snapshot per call is what a caller wants between
# statements and not within one: a statement that spans
# several calls -- an index lookup is a scan and then a read
# per row -- would otherwise see a different committed state
# in each, and could return a mixture that never existed.
# sm1_stmt_begin says where a statement starts, and every read
# after it uses one snapshot. The boundary is not the call
# itself: the snapshot is taken by the first read after it, so
# a statement may wait before it reads and still see
# everything committed by the time it does.

pairings rr
session T1 rc
session W rr
seed R1 0

@1 T1 begin
@2 T1 stmt
@3 W begin
@4 W write R1 v2
@4 W insert R2 new
@5 W commit
@6 T1 scan R1 R9
@6 T1 read R1
@7 check last T1 v2
@7 W begin
@8 W write R1 v3
@9 W commit
@10 T1 scan R1 R9
@10 T1 read R1
@11 check last T1 v2
@12 T1 stmt
@13 T1 read R1
@14 check last T1 v3
@15 T1 commit
@16 check row R1 v3
