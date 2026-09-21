# a08 -- a transaction reads its own writes.
#
# Its inserts, updates and deletes are visible to it before
# commit, through both read paths, including a row it inserted
# and then overwrote and deleted.

pairings all
session T1
seed R1 0
seed R2 0

@1 T1 begin
@2 T1 insert R3 t1
@2 T1 write R1 t1
@2 T1 delete R2
@3 T1 read R1
@3 T1 read R2
@3 T1 read R3
@3 T1 scan R1 R3
@4 T1 write R3 again
@5 T1 read R3
@6 T1 delete R3
@7 T1 read R3
@7 T1 scan R1 R3
@8 T1 commit
@9 check row R1 t1
@9 check row R2 -
@9 check row R3 -
@9 check rows
