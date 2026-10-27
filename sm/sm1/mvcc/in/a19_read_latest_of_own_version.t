# a19 -- above read committed, a read of a key's newest version
# reads the transaction's own version, and nothing else.
#
# sm1_read_latest looks past the snapshot, which only read
# committed has to look past: at repeatable read and serializable
# the snapshot is the transaction's for its life. A version the
# transaction wrote itself is newer than its snapshot and is no
# other's to change, so it is read at every level: an insert's
# arbiter so finds a key its own statement took, where the
# statement's own view does not show it (dg WORKNOTE.md step 306).
#
# For each of R (repeatable read) and S (serializable), on rows of
# its own, and C (read committed) as before:
# 1. In a statement, write the first row: the statement reads it as
#    it was, and readlatest reads the new value.
# 2. Lock the second row, which it has not written: readlatest is
#    invalid above read committed, and reads the row at it.

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

@1 R begin
@2 R stmt
@3 R write R1 a2
@4 R read R1
@5 R readlatest R1
@6 R lock R2 shared
@7 R readlatest R2
@8 R abort
@9 S begin
@10 S stmt
@11 S write R3 a2
@12 S read R3
@13 S readlatest R3
@14 S lock R4 shared
@15 S readlatest R4
@16 S abort
@17 C begin
@18 C stmt
@19 C write R5 a2
@20 C read R5
@21 C readlatest R5
@22 C lock R6 shared
@23 C readlatest R6
@24 C abort
