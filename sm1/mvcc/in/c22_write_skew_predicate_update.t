# c22 -- write skew over a predicate, resolved by updates.
#
# The doctors-on-call schedule: each transaction scans the
# range to check that someone else is on call, then takes
# itself off by updating its own row. Neither write changes
# the membership of the range, as c03's inserts and c04's
# deletes do, so this is the case where the edge can only come
# from a value written inside a range somebody read.

pairings all
session T1
session T2
seed R1 1
seed R2 1

@1 T1 begin
@1 T2 begin
@2 T1 scan R1 R2
@3 T2 scan R1 R2
@4 T1 write R1 0
@5 T2 write R2 0
@6 T1 commit?
@7 T2 commit?
@8 check committed T1 T2 when rr
@8 check committed T1 T2 when rr+s
@8 check at-most-one-committed T1 T2 when s+s
@9 check rows when rr
@9 check rows when rr+s
