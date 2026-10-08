# c34 -- a transaction declared read-only refuses to write.
#
# sm1 enforces the declaration at every level: the read-only
# rule of §7.3 is sound only for a transaction that cannot
# write. The refused write is not an abort; T1 reads on and
# commits, and the row is unchanged.

pairings all
session T1 ro
seed R1 0

@1 T1 begin
@2 T1 write R1 t1
@3 T1 read R1
@4 T1 commit
@5 check committed T1
@6 check row R1 0
