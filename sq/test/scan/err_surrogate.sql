-- Verify that half a surrogate pair is refused at the half.
-- 1. A first surrogate followed by an ordinary character.
SELECT E'\ud800x';
