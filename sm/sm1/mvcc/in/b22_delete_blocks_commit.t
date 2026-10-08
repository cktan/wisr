# b22 -- a delete waiting on a live writer that commits.
#
# b05 has a delete meet a delete that has already committed,
# so it fails at once. Here the first delete is still open,
# which is the other half: a delete takes the same exclusive
# row lock a write does, so the second one waits rather than
# reading through. When the holder commits, the waiter is
# judged by first-committer-wins and fails retryably -- the
# row it meant to delete is gone, but it is not told
# notfound, because the honest answer is that it lost a race.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 delete R1
@3 T2 delete R1 blocks
@4 T1 commit
@5 check last T2 conflict
@6 check committed T1
@7 check row R1 -
