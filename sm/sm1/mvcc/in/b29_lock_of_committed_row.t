# b29 -- locking a row that changed after the snapshot.
#
# A lock is only worth taking if what the caller reads under
# it is current, so sm1_lock runs first-committer-wins once the
# lock is held: a row committed after the transaction began
# fails the call rather than handing back a stale row under a
# lock that suggests otherwise. It is the b01 answer for a
# call that writes nothing, and it holds in both modes.

pairings all
session T1
session T2
session T3
seed R1 0

@1 T1 begin
@1 T3 begin
@2 T1 read R1
@3 T2 begin
@4 T2 write R1 t2
@5 T2 commit
@6 T1 lock R1 shared
@7 check last T1 conflict
@8 T3 lock R1 exclusive
@9 check last T3 conflict
@10 check row R1 t2
