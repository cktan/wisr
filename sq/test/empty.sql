-- Verify that input with no statement in it parses to nothing.
-- 1. Comments, of both kinds, and stray semicolons only.
/* a block comment /* nested */ still a comment */
;;
