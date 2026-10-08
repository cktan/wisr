# b35 -- two read-modify-writes of one row; the first commits.
#
# R1 holds v = 0. T1 and T2 both read it; T1 sets v = v + 10,
# then T2 sets v = v + 20 and waits, T1's uncommitted version
# being the lock. T1 commits, and T2's outcome depends on its
# own level, at every pairing of the three levels:
#
# - READ COMMITTED: the write is refused with updated, not an
#   abort. T2 reads the newest value, 10, re-applies v + 20 to
#   it, and writes 30: both increments survive, as PostgreSQL's
#   EvalPlanQual re-check gives (SM1_DESIGN.md §5.4).
# - REPEATABLE READ and SERIALIZABLE: first committer wins. T2
#   fails with conflict and is aborted; R1 keeps T1's 10.
#
# T1's level does not change it. The scenario language has no
# arithmetic, so v + 10 and v + 20 are written as the values
# they give from 0.

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
@6 T1 commit
@7 check committed T1
@8 check last T2 updated when t2=rc
@9 T2 readlatest R1 when t2=rc
@10 check last T2 10 when t2=rc
@11 T2 write R1 30 when t2=rc
@12 T2 commit when t2=rc
@13 check committed T2 when t2=rc
@14 check row R1 30 when t2=rc
@8 check last T2 conflict when t2=rr+ser
@14 check row R1 10 when t2=rr+ser
