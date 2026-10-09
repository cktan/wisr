-- Verify that sq refuses this as PostgreSQL does, with its message:
-- CREATE OR REPLACE CONSTRAINT TRIGGER is not supported
-- 1. Parse the one statement below, which the grammar accepts as far
--    as its rules go and one of its actions refuses.
CREATE OR REPLACE CONSTRAINT TRIGGER t AFTER INSERT ON x FOR EACH ROW EXECUTE FUNCTION f();
