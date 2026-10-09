-- Verify that operators scan as PostgreSQL scans them.
-- 1. The multi-character tokens the grammar names.
-- 2. User operators, and how a trailing + or - is given back.
-- 3. A comment start inside what looks like an operator.
-- 4. Nested block comments, which scan to nothing.
SELECT a::int, b := 1, c => 2, d <= e, f >= g, h <> i, j != k, l .. m;
SELECT a +- b, a @> b, a <-> b, a !~~* b, a *- b, a @- b, a ~~ b;
SELECT a <--comment
  b, a +/* c */ b;
/* outer /* inner */ still outer */ SELECT 1;
