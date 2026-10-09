-- Verify that an escape that makes invalid UTF-8 is refused, naming the bytes.
-- 1. An escape string holding the single byte 0xff.
SELECT E'\xff';
