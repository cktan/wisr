-- Verify that a malformed Unicode escape is refused with its hint.
-- 1. A \u escape with two hexadecimal digits where four are needed.
SELECT E'\u00';
