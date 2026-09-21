# b15 -- a write after the reader is done.
#
# Nothing to detect: this is the serial history T1, T2. A level
# that conflicted here would make read-then-write impossible.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@2 T1 read R1
@3 T1 commit
@4 T2 begin
@5 T2 write R1 t2
@6 T2 commit
@7 check committed T1 T2
@8 check row R1 t2
