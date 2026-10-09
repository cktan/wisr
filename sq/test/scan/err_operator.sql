-- Verify that an operator of 64 characters or more is refused.
-- 1. An operator of 64 characters.
SELECT a @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ b;
