# h06 -- writers share a table; a builder waits for them all.
#
# T1 and T2 both take IX, as writers of one table do, each in
# a slot; IX and IX are compatible, so neither waits. T3's S,
# as an index build takes it, conflicts with IX: it sweeps both
# slots into the table and waits for both writers.

pairings rr
session T1
session T2
session T3

@1 T1 begin
@1 T2 begin
@1 T3 begin
@2 T1 lockoid 1 ix
@3 T2 lockoid 1 ix
@4 T3 lockoid 1 shared blocks
@5 T1 commit
@6 T2 commit
@7 check last T3 ok
@8 T3 commit
@9 check committed T1 T2 T3
