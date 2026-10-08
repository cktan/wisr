# c33 -- a transaction declared read-only spares a pivot.
#
# T2 reads R1 and T1 overwrites it and commits: T2 --rw--> T1.
# T3 reads R2 and T2 overwrites it: T3 --rw--> T2, T2 the pivot,
# with T1 committed first. T1 committed after T3's snapshot, and
# T3 is declared read-only, so T3, T2, T1 is a serial order and
# all three commit. Undeclared, T3 could still write when T2
# commits, and T2 would fail (Ports and Grittner's read-only
# rule, SM1_DESIGN.md §7.3; PostgreSQL's receipt-report, p02).

pairings all
session T1
session T2
session T3 ro
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@1 T3 begin
@2 T2 read R1
@3 T1 write R1 t1
@4 T1 commit
@5 T3 read R2
@6 T2 write R2 t2
@7 T2 commit
@8 T3 commit
@9 check committed T1 T2 T3
@10 check row R1 t1
@10 check row R2 t2
