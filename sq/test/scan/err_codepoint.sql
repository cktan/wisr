-- Verify that a Unicode escape past the last code point is refused.
-- 1. A \U escape for U+110000.
SELECT E'\U00110000';
