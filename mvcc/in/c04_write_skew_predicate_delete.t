# c04 -- write skew over a predicate, resolved by deletes.
#
# "Remove one, as long as one remains", run twice, emptying the
# set. At RR the invariant breaks and both commit; the stress
# scenario e02 shows SERIALIZABLE holding the same invariant
# under load.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T1 scan R1 R3
@3 T2 scan R1 R3
@4 T1 delete R1
@5 T2 delete R2
@6 T1 commit?
@7 T2 commit?
@8 check committed T1 T2 when rr
@8 check committed T1 T2 when rr+v
@8 check at-most-one-committed T1 T2 when v+v
@9 check rows when rr
