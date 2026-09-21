# a09 -- a cursor does not see writes made after it opened.
#
# T1 opens a cursor, then updates each row the cursor returns
# and inserts another inside the range. The cursor must go on
# returning the rows as they were, and must never return R4.
# This is what makes "scan a range and update every row in it"
# terminate.

pairings all
session T1
seed R1 0
seed R2 0
seed R3 0

@1 T1 begin
@2 T1 open C R1 R4
@3 T1 next C
@4 T1 write R1 t1
@4 T1 insert R4 t1
@5 T1 next C
@6 T1 write R2 t1
@7 T1 next C
@8 T1 write R3 t1
@9 T1 next C
@10 T1 close C
@11 T1 scan R1 R4
@12 T1 commit
@13 check rows
