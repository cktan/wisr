# b28 -- two shared holders that both want to write.
#
# b16 needs two rows to make a cycle, because an exclusive
# lock is held by one transaction and the second waits for
# the first. A shared lock has many holders, so one row is
# enough: T1 and T2 both hold R1, both ask to upgrade, and
# each is waiting for the other to let go of the very lock it
# holds. This is what every SELECT ... FOR SHARE followed by
# an UPDATE risks, and the engine breaks it the same way --
# the younger, T2, is the victim, and T1's upgrade completes.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 lock R1 shared
@3 T2 lock R1 shared
@4 T1 write R1 t1 blocks
@5 T2 write R1 t2
@6 check last T2 deadlock
@7 check last T1 ok
@8 T1 commit
@9 check committed T1
@10 check row R1 t1
