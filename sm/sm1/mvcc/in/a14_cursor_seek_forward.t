# a14 -- a cursor moved forward keeps the view it opened with.
#
# A reposition is not a reopen. T1 opens a cursor, reads one
# row, then seeks past two more to R5, and the rows it skipped
# are simply not returned -- no error, no rewind. What it does
# return is still the state at open: a value written after the
# cursor existed does not show, exactly as a09 has it for
# stepping. Seeking backwards is refused, so a cursor only
# ever moves one way through its range.

pairings all
session T1
seed R1 1
seed R3 3
seed R5 5
seed R7 7

@1 T1 begin
@2 T1 open C R1 R9
@3 T1 next C
@4 T1 write R5 later
@4 T1 write R7 later
@5 T1 seek C R5
@6 T1 next C
@7 T1 seek C R1
@8 check last T1 invalid
@9 T1 next C
@10 T1 close C
@11 T1 commit
@12 check row R5 later
@12 check row R7 later
