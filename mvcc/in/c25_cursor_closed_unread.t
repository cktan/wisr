# c25 -- a cursor closed without reading anything.
#
# The limit of c24's narrowing: a cursor that never called
# next examined no key, so it read nothing and its predicate
# goes away entirely. T2 may then write anywhere in the range
# it asked for, and only T2 --rw--> T1 remains -- one edge,
# and not a cycle.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T1 open C R1 R5
@3 T1 close C
@4 T2 write R1 t2
@5 T2 read R2
@6 T1 write R2 t1
@7 T1 commit
@8 T2 commit
@9 check committed T1 T2
@10 check rows
