-- Verify that a block comment never closed is refused.
-- 1. A nested comment closed once only.
SELECT 1 /* outer /* inner */ still open;
