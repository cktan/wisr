# b36 -- two read-modify-writes of one row; the first aborts.
#
# The other half of b35. R1 holds v = 0; T1 and T2 both read
# it, T1 sets v = v + 10, and T2's v = v + 20 waits for T1. T1
# aborts, so its version is dead and nothing committed since
# T2 began: T2's write goes through at once, at every pairing of
# the three levels, with no re-read, and R1 ends at 20.

pairings levels
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 read R1
@3 T2 read R1
@4 T1 write R1 10
@5 T2 write R1 20 blocks
@6 T1 abort
@7 check last T2 ok
@8 T2 commit
@9 check committed T2
@10 check row R1 20
