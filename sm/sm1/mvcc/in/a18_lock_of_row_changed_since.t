# a18 -- a row lock of a row the statement read refuses one the
# transaction changed since the statement began.
#
# A statement does not see its own transaction's writes made
# after it began (a16's mark, SM1_DESIGN.md §6.2), and a write of
# such a row fails with selfmodified. A lock taken as of a row the
# statement read -- SQL's FOR UPDATE, a cursor's FETCH after a
# later UPDATE or DELETE of its transaction (dg WORKNOTE.md step
# 305) -- is refused the same way, at every level, and the row is
# passed over: PostgreSQL's TM_SelfModified. A plain lock, which a
# caller takes to read the newest version, its own included, is
# not.
#
# For each of C (read committed), R (repeatable read) and S
# (serializable), on rows of its own:
# 1. In a statement, update the first row and delete the second.
# 2. Lock each as read: selfmodified, both.
# 3. Lock the first plainly: ok.
# 4. In a new statement, lock the first as read: ok, as it was
#    changed before that statement began; the second, deleted
#    then, is locked too, as a key need not exist.

pairings rr
session C rc
session R rr
session S ser
seed R1 a
seed R2 b
seed R3 a
seed R4 b
seed R5 a
seed R6 b

@1 C begin
@2 C stmt
@3 C write R1 a2
@4 C delete R2
@5 C lockread R1 exclusive
@6 C lockread R2 shared
@7 C lock R1 exclusive
@8 C stmt
@9 C lockread R1 exclusive
@10 C lockread R2 exclusive
@11 C commit
@12 R begin
@13 R stmt
@14 R write R3 a2
@15 R delete R4
@16 R lockread R3 exclusive
@17 R lockread R4 shared
@18 R lock R3 exclusive
@19 R stmt
@20 R lockread R3 exclusive
@21 R lockread R4 exclusive
@22 R commit
@23 S begin
@24 S stmt
@25 S write R5 a2
@26 S delete R6
@27 S lockread R5 exclusive
@28 S lockread R6 shared
@29 S lock R5 exclusive
@30 S stmt
@31 S lockread R5 exclusive
@32 S lockread R6 exclusive
@33 S commit
@34 check row R1 a2
@34 check row R3 a2
@34 check row R5 a2
