-- PostgreSQL alone, before the load: a value out of line is stored
-- uncompressed, as sm stores it (DESIGN.md section 4).
alter table toasted alter column body set storage external;
