# c29 -- a rolled-back read still constrains the transaction.
#
# c02's write skew, with T1's read of R1 made before a
# savepoint it then rolls back to. The rollback deletes the
# version T1 wrote after the mark, but deliberately does not
# touch the SIREAD it took: undoing a write is cheap and
# exact, while undoing a read would mean proving nothing was
# decided by it. So the edge T1 --rw--> T2 still forms, the
# cycle still closes, and S/S still refuses the pair. If a
# rollback did launder the read set, both would commit here.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T1 savepoint
@3 T1 read R1
@4 T1 write R2 scratch
@5 T1 rollback 1
@6 check row R2 0
@7 T2 read R2
@8 T2 write R1 t2
@9 T1 write R2 t1
@10 T1 commit?
@10 T2 commit?
@11 check committed T1 T2 when rr
@11 check committed T1 T2 when rr+s
@11 check at-most-one-committed T1 T2 when s+s
