-- Verify that a parameter number must fit in 32 bits.
-- 1. A parameter numbered past the largest integer.
SELECT $99999999999;
