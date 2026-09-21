# a10 -- a row deleted and inserted again.
#
# The tombstone and the new version both sit under R1. T2,
# whose snapshot predates the commit, must resolve to the old
# value, and exactly once in a scan.

pairings all
session T1
session T2
seed R1 0

@1 T2 begin
@2 T2 read R1
@3 T1 begin
@4 T1 delete R1
@4 T1 insert R1 new
@5 T1 read R1
@6 T1 commit
@7 T2 read R1
@7 T2 scan R1 R2
@8 T2 commit
@9 check rows
