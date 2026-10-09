-- Verify that a schema-qualified type with a modifier and array bounds parses.
-- 1. A qualified name, two modifiers, and two array dimensions.
pg_catalog.numeric(10, 2)[][3]
