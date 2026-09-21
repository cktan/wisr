# a15 -- read committed sees a commit between statements; the
# levels above it do not.
#
# The suite's other scripts run at repeatable read and above,
# where one snapshot serves the whole transaction. Read
# committed takes a fresh one per call, which is the level's
# definition rather than a defect: a non-repeatable read and a
# phantom are both permitted between statements, and both
# appear here. T2 runs beside it at repeatable read with the
# same steps and sees neither, which is what makes the
# difference the level's and not the timing's.

pairings rr
session T1 rc
session T2 rr
session W rr
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 scan R1 R9
@2 T1 read R1
@3 check last T1 0
@3 W begin
@4 W write R1 v2
@4 W insert R2 new
@5 W commit
@6 T1 scan R1 R9
@6 T1 read R1
@7 check last T1 v2
@8 T2 scan R1 R9
@8 T2 read R1
@9 check last T2 0
@10 T1 commit
@10 T2 commit
@11 check row R1 v2
@11 check row R2 new
