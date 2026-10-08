# b11 -- two inserts of the same absent row.
#
# An insert takes the row lock on a key that does not exist
# yet, so two inserters serialize on it rather than both
# succeeding.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 insert R2 t1
@3 T2 insert R2 t2 blocks
@4 T1 commit
@5 check last T2 conflict
@6 check row R2 t1
