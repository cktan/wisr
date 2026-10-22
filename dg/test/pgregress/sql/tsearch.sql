-- directory paths are passed to us in environment variables
\getenv abs_srcdir PG_ABS_SRCDIR

--
-- Sanity checks for text search catalogs
--
-- NB: we assume the oidjoins test will have caught any dangling links,
-- that is OID or REGPROC fields that are not zero and do not match some
-- row in the linked-to table.  However, if we want to enforce that a link
-- field can't be 0, we have to check it here.

-- Find unexpected zero link entries

--dg-skip: relation "?" does not exist
--dg SELECT oid, prsname
--dg FROM pg_ts_parser
--dg WHERE prsnamespace = 0 OR prsstart = 0 OR prstoken = 0 OR prsend = 0 OR
--dg       -- prsheadline is optional
--dg       prslextype = 0;

--dg-skip: relation "?" does not exist
--dg SELECT oid, dictname
--dg FROM pg_ts_dict
--dg WHERE dictnamespace = 0 OR dictowner = 0 OR dicttemplate = 0;

--dg-skip: relation "?" does not exist
--dg SELECT oid, tmplname
--dg FROM pg_ts_template
--dg WHERE tmplnamespace = 0 OR tmpllexize = 0;  -- tmplinit is optional

--dg-skip: relation "?" does not exist
--dg SELECT oid, cfgname
--dg FROM pg_ts_config
--dg WHERE cfgnamespace = 0 OR cfgowner = 0 OR cfgparser = 0;

--dg-skip: relation "?" does not exist
--dg SELECT mapcfg, maptokentype, mapseqno
--dg FROM pg_ts_config_map
--dg WHERE mapcfg = 0 OR mapdict = 0;

-- Look for pg_ts_config_map entries that aren't one of parser's token types
--dg-skip: relation "?" does not exist
--dg SELECT * FROM
--dg   ( SELECT oid AS cfgid, (ts_token_type(cfgparser)).tokid AS tokid
--dg     FROM pg_ts_config ) AS tt
--dg RIGHT JOIN pg_ts_config_map AS m
--dg     ON (tt.cfgid=m.mapcfg AND tt.tokid=m.maptokentype)
--dg WHERE
--dg     tt.cfgid IS NULL OR tt.tokid IS NULL;

-- Load some test data
--dg-skip: type "?" does not exist
--dg CREATE TABLE test_tsvector(
--dg 	t text,
--dg 	a tsvector
--dg );

\set filename :abs_srcdir '/data/tsearch.data'
--dg-cascade
--dg COPY test_tsvector FROM :'filename';

--dg-cascade
--dg ANALYZE test_tsvector;

-- test basic text search behavior without indexes, then with

--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wr&qh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'eq&yt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'eq|yt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '(eq&yt)|(wr&qh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '(eq|yt)&(wr|qh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'w:*|q:*';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ any ('{wr,qh}');
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'no_such_lexeme';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!no_such_lexeme';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'pl <-> yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'yh <-> pl';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'qe <2> qt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!pl <-> yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!pl <-> !yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!yh <-> pl';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!qe <2> qt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(pl <-> yh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(yh <-> pl)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(qe <2> qt)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wd:A';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wd:D';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!wd:A';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!wd:D';

--dg-cascade
--dg create index wowidx on test_tsvector using gist (a);

SET enable_seqscan=OFF;
SET enable_indexscan=ON;
SET enable_bitmapscan=OFF;

--dg-cascade
--dg explain (costs off) SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';

--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wr&qh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'eq&yt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'eq|yt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '(eq&yt)|(wr&qh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '(eq|yt)&(wr|qh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'w:*|q:*';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ any ('{wr,qh}');
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'no_such_lexeme';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!no_such_lexeme';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'pl <-> yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'yh <-> pl';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'qe <2> qt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!pl <-> yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!pl <-> !yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!yh <-> pl';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!qe <2> qt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(pl <-> yh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(yh <-> pl)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(qe <2> qt)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wd:A';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wd:D';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!wd:A';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!wd:D';

SET enable_indexscan=OFF;
SET enable_bitmapscan=ON;

--dg-cascade
--dg explain (costs off) SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';

--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wr&qh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'eq&yt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'eq|yt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '(eq&yt)|(wr&qh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '(eq|yt)&(wr|qh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'w:*|q:*';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ any ('{wr,qh}');
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'no_such_lexeme';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!no_such_lexeme';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'pl <-> yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'yh <-> pl';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'qe <2> qt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!pl <-> yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!pl <-> !yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!yh <-> pl';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!qe <2> qt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(pl <-> yh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(yh <-> pl)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(qe <2> qt)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wd:A';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wd:D';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!wd:A';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!wd:D';

-- Test siglen parameter of GiST tsvector_ops
--dg-cascade
--dg CREATE INDEX wowidx1 ON test_tsvector USING gist (a tsvector_ops(foo=1));
--dg-cascade
--dg CREATE INDEX wowidx1 ON test_tsvector USING gist (a tsvector_ops(siglen=0));
--dg-cascade
--dg CREATE INDEX wowidx1 ON test_tsvector USING gist (a tsvector_ops(siglen=2048));
--dg-cascade
--dg CREATE INDEX wowidx1 ON test_tsvector USING gist (a tsvector_ops(siglen=100,foo='bar'));
--dg-cascade
--dg CREATE INDEX wowidx1 ON test_tsvector USING gist (a tsvector_ops(siglen=100, siglen = 200));

--dg-cascade
--dg CREATE INDEX wowidx2 ON test_tsvector USING gist (a tsvector_ops(siglen=1));

--dg-cascade
--dg \d test_tsvector

--dg-cascade
--dg DROP INDEX wowidx;

--dg-cascade
--dg EXPLAIN (costs off) SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';

--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wr&qh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'eq&yt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'eq|yt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '(eq&yt)|(wr&qh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '(eq|yt)&(wr|qh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'w:*|q:*';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ any ('{wr,qh}');
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'no_such_lexeme';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!no_such_lexeme';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'pl <-> yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'yh <-> pl';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'qe <2> qt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!pl <-> yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!pl <-> !yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!yh <-> pl';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!qe <2> qt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(pl <-> yh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(yh <-> pl)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(qe <2> qt)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wd:A';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wd:D';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!wd:A';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!wd:D';

--dg-cascade
--dg DROP INDEX wowidx2;

--dg-cascade
--dg CREATE INDEX wowidx ON test_tsvector USING gist (a tsvector_ops(siglen=484));

--dg-cascade
--dg \d test_tsvector

--dg-cascade
--dg EXPLAIN (costs off) SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';

--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wr&qh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'eq&yt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'eq|yt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '(eq&yt)|(wr&qh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '(eq|yt)&(wr|qh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'w:*|q:*';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ any ('{wr,qh}');
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'no_such_lexeme';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!no_such_lexeme';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'pl <-> yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'yh <-> pl';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'qe <2> qt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!pl <-> yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!pl <-> !yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!yh <-> pl';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!qe <2> qt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(pl <-> yh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(yh <-> pl)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(qe <2> qt)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wd:A';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wd:D';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!wd:A';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!wd:D';

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;

--dg-cascade
--dg DROP INDEX wowidx;

--dg-cascade
--dg ALTER TABLE test_tsvector SET (parallel_workers = 2);
--dg-cascade
--dg CREATE INDEX wowidx ON test_tsvector USING gin (a);

SET enable_seqscan=OFF;
-- GIN only supports bitmapscan, so no need to test plain indexscan

--dg-cascade
--dg explain (costs off) SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';

--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wr|qh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wr&qh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'eq&yt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'eq|yt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '(eq&yt)|(wr&qh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '(eq|yt)&(wr|qh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'w:*|q:*';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ any ('{wr,qh}');
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'no_such_lexeme';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!no_such_lexeme';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'pl <-> yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'yh <-> pl';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'qe <2> qt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!pl <-> yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!pl <-> !yh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!yh <-> pl';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!qe <2> qt';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(pl <-> yh)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(yh <-> pl)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!(qe <2> qt)';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wd:A';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wd:D';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!wd:A';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!wd:D';

-- Test optimization of non-empty GIN_SEARCH_MODE_ALL queries
--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!qh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ '!qh';

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wr' AND a @@ '!qh';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ 'wr' AND a @@ '!qh';

RESET enable_seqscan;

--dg-cascade
--dg INSERT INTO test_tsvector VALUES ('???', 'DFG:1A,2B,6C,10 FGH');
--dg-cascade
--dg SELECT * FROM ts_stat('SELECT a FROM test_tsvector') ORDER BY ndoc DESC, nentry DESC, word LIMIT 10;
--dg-cascade
--dg SELECT * FROM ts_stat('SELECT a FROM test_tsvector', 'AB') ORDER BY ndoc DESC, nentry DESC, word;

--dictionaries and to_tsvector

--dg-skip: function ?(...) does not exist
--dg SELECT ts_lexize('english_stem', 'skies');
--dg-skip: function ?(...) does not exist
--dg SELECT ts_lexize('english_stem', 'identity');

--dg-skip: function ?(...) does not exist
--dg SELECT * FROM ts_token_type('default');

--dg-skip: function ?(...) does not exist
--dg SELECT * FROM ts_parse('default', '345 qwe@efd.r '' http://www.com/ http://aew.werc.ewr/?ad=qwe&dw 1aew.werc.ewr/?ad=qwe&dw 2aew.werc.ewr http://3aew.werc.ewr/?ad=qwe&dw http://4aew.werc.ewr http://5aew.werc.ewr:8100/?  ad=qwe&dw 6aew.werc.ewr:8100/?ad=qwe&dw 7aew.werc.ewr:8100/?ad=qwe&dw=%20%32 +4.0e-10 qwe qwe qwqwe 234.435 455 5.005 teodor@stack.net teodor@123-stack.net 123_teodor@stack.net 123-teodor@stack.net qwe-wer asdf <fr>qwer jf sdjk<we hjwer <werrwe> ewr1> ewri2 <a href="qwe<qwe>">
--dg /usr/local/fff /awdf/dwqe/4325 rewt/ewr wefjn /wqe-324/ewr gist.h gist.h.c gist.c. readline 4.2 4.2. 4.2, readline-4.2 readline-4.2. 234
--dg <i <b> wow  < jqw <> qwerty');

--dg-skip: function ?(...) does not exist
--dg SELECT to_tsvector('english', '345 qwe@efd.r '' http://www.com/ http://aew.werc.ewr/?ad=qwe&dw 1aew.werc.ewr/?ad=qwe&dw 2aew.werc.ewr http://3aew.werc.ewr/?ad=qwe&dw http://4aew.werc.ewr http://5aew.werc.ewr:8100/?  ad=qwe&dw 6aew.werc.ewr:8100/?ad=qwe&dw 7aew.werc.ewr:8100/?ad=qwe&dw=%20%32 +4.0e-10 qwe qwe qwqwe 234.435 455 5.005 teodor@stack.net teodor@123-stack.net 123_teodor@stack.net 123-teodor@stack.net qwe-wer asdf <fr>qwer jf sdjk<we hjwer <werrwe> ewr1> ewri2 <a href="qwe<qwe>">
--dg /usr/local/fff /awdf/dwqe/4325 rewt/ewr wefjn /wqe-324/ewr gist.h gist.h.c gist.c. readline 4.2 4.2. 4.2, readline-4.2 readline-4.2. 234
--dg <i <b> wow  < jqw <> qwerty');

--dg-skip: function ?(...) does not exist
--dg SELECT length(to_tsvector('english', '345 qwe@efd.r '' http://www.com/ http://aew.werc.ewr/?ad=qwe&dw 1aew.werc.ewr/?ad=qwe&dw 2aew.werc.ewr http://3aew.werc.ewr/?ad=qwe&dw http://4aew.werc.ewr http://5aew.werc.ewr:8100/?  ad=qwe&dw 6aew.werc.ewr:8100/?ad=qwe&dw 7aew.werc.ewr:8100/?ad=qwe&dw=%20%32 +4.0e-10 qwe qwe qwqwe 234.435 455 5.005 teodor@stack.net teodor@123-stack.net 123_teodor@stack.net 123-teodor@stack.net qwe-wer asdf <fr>qwer jf sdjk<we hjwer <werrwe> ewr1> ewri2 <a href="qwe<qwe>">
--dg /usr/local/fff /awdf/dwqe/4325 rewt/ewr wefjn /wqe-324/ewr gist.h gist.h.c gist.c. readline 4.2 4.2. 4.2, readline-4.2 readline-4.2. 234
--dg <i <b> wow  < jqw <> qwerty'));

-- ts_debug

--dg-skip: function ?(...) does not exist
--dg SELECT * from ts_debug('english', '<myns:foo-bar_baz.blurfl>abc&nm1;def&#xa9;ghi&#245;jkl</myns:foo-bar_baz.blurfl>');

-- check parsing of URLs
--dg-skip: function ?(...) does not exist
--dg SELECT * from ts_debug('english', 'http://www.harewoodsolutions.co.uk/press.aspx</span>');
--dg-skip: function ?(...) does not exist
--dg SELECT * from ts_debug('english', 'http://aew.wer0c.ewr/id?ad=qwe&dw<span>');
--dg-skip: function ?(...) does not exist
--dg SELECT * from ts_debug('english', 'http://5aew.werc.ewr:8100/?');
--dg-skip: function ?(...) does not exist
--dg SELECT * from ts_debug('english', '5aew.werc.ewr:8100/?xx');
--dg-skip: function ?(...) does not exist
--dg SELECT token, alias,
--dg   dictionaries, dictionaries is null as dnull, array_dims(dictionaries) as ddims,
--dg   lexemes, lexemes is null as lnull, array_dims(lexemes) as ldims
--dg from ts_debug('english', 'a title');

-- to_tsquery

--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', 'qwe & sKies ');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('simple', 'qwe & sKies ');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '''the wether'':dc & ''           sKies '':BC ');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', 'asd&(and|fghj)');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '(asd&and)|fghj');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '(asd&!and)|fghj');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '(the|and&(i&1))&fghj');

--dg-skip: function ?(...) does not exist
--dg SELECT plainto_tsquery('english', 'the and z 1))& fghj');
--dg-skip: function ?(...) does not exist
--dg SELECT plainto_tsquery('english', 'foo bar') && plainto_tsquery('english', 'asd');
--dg-skip: function ?(...) does not exist
--dg SELECT plainto_tsquery('english', 'foo bar') || plainto_tsquery('english', 'asd fg');
--dg-skip: function ?(...) does not exist
--dg SELECT plainto_tsquery('english', 'foo bar') || !!plainto_tsquery('english', 'asd fg');
--dg-skip: function ?(...) does not exist
--dg SELECT plainto_tsquery('english', 'foo bar') && 'asd | fg';

-- Check stop word deletion, a and s are stop-words
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '!(a & !b) & c');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '!(a & !b)');

--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '(1 <-> 2) <-> a');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '(1 <-> a) <-> 2');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '(a <-> 1) <-> 2');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', 'a <-> (1 <-> 2)');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '1 <-> (a <-> 2)');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '1 <-> (2 <-> a)');

--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '(1 <-> 2) <3> a');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '(1 <-> a) <3> 2');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '(a <-> 1) <3> 2');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', 'a <3> (1 <-> 2)');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '1 <3> (a <-> 2)');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '1 <3> (2 <-> a)');

--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '(1 <3> 2) <-> a');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '(1 <3> a) <-> 2');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '(a <3> 1) <-> 2');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', 'a <-> (1 <3> 2)');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '1 <-> (a <3> 2)');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '1 <-> (2 <3> a)');

--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '((a <-> 1) <-> 2) <-> s');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '(2 <-> (a <-> 1)) <-> s');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '((1 <-> a) <-> 2) <-> s');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '(2 <-> (1 <-> a)) <-> s');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', 's <-> ((a <-> 1) <-> 2)');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', 's <-> (2 <-> (a <-> 1))');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', 's <-> ((1 <-> a) <-> 2)');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', 's <-> (2 <-> (1 <-> a))');

--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '((a <-> 1) <-> s) <-> 2');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '(s <-> (a <-> 1)) <-> 2');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '((1 <-> a) <-> s) <-> 2');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '(s <-> (1 <-> a)) <-> 2');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '2 <-> ((a <-> 1) <-> s)');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '2 <-> (s <-> (a <-> 1))');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '2 <-> ((1 <-> a) <-> s)');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '2 <-> (s <-> (1 <-> a))');

--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', 'foo <-> (a <-> (the <-> bar))');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', '((foo <-> a) <-> the) <-> bar');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('english', 'foo <-> a <-> the <-> bar');
--dg-skip: function ?(...) does not exist
--dg SELECT phraseto_tsquery('english', 'PostgreSQL can be extended by the user in many ways');


--dg-skip: function ?(...) does not exist
--dg SELECT ts_rank_cd(to_tsvector('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg '), to_tsquery('english', 'paint&water'));

--dg-skip: function ?(...) does not exist
--dg SELECT ts_rank_cd(to_tsvector('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg '), to_tsquery('english', 'breath&motion&water'));

--dg-skip: function ?(...) does not exist
--dg SELECT ts_rank_cd(to_tsvector('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg '), to_tsquery('english', 'ocean'));

--dg-skip: function ?(...) does not exist
--dg SELECT ts_rank_cd(to_tsvector('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg '), to_tsquery('english', 'painted <-> Ship'));

--dg-skip: function ?(...) does not exist
--dg SELECT ts_rank_cd(strip(to_tsvector('both stripped')),
--dg                   to_tsquery('both & stripped'));

--dg-skip: function ?(...) does not exist
--dg SELECT ts_rank_cd(to_tsvector('unstripped') || strip(to_tsvector('stripped')),
--dg                   to_tsquery('unstripped & stripped'));

--headline tests
--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg ', to_tsquery('english', 'paint&water'));

--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg ', to_tsquery('english', 'breath&motion&water'));

--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg ', to_tsquery('english', 'ocean'));

--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg ', to_tsquery('english', 'day & drink'));

--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg ', to_tsquery('english', 'day | drink'));

--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg ', to_tsquery('english', 'day | !drink'));

--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg ', to_tsquery('english', 'painted <-> Ship & drink'));

--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg ', to_tsquery('english', 'painted <-> Ship | drink'));

--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg ', to_tsquery('english', 'painted <-> Ship | !drink'));

--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg ', phraseto_tsquery('english', 'painted Ocean'));

--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg ', phraseto_tsquery('english', 'idle as a painted Ship'));

--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english',
--dg 'Lorem ipsum urna.  Nullam nullam ullamcorper urna.',
--dg to_tsquery('english','Lorem') && phraseto_tsquery('english','ullamcorper urna'),
--dg 'MaxWords=100, MinWords=1');

--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english',
--dg 'Lorem ipsum urna.  Nullam nullam ullamcorper urna.',
--dg phraseto_tsquery('english','ullamcorper urna'),
--dg 'MaxWords=100, MinWords=5');

--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english', '
--dg <html>
--dg <!-- some comment -->
--dg <body>
--dg Sea view wow <u>foo bar</u> <i>qq</i>
--dg <a href="http://www.google.com/foo.bar.html" target="_blank">YES &nbsp;</a>
--dg ff-bg
--dg <script>
--dg        document.write(15);
--dg </script>
--dg </body>
--dg </html>',
--dg to_tsquery('english', 'sea&foo'), 'HighlightAll=true');

--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('simple', '1 2 3 1 3'::text, '1 <-> 3', 'MaxWords=2, MinWords=1');
--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('simple', '1 2 3 1 3'::text, '1 & 3', 'MaxWords=4, MinWords=1');
--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('simple', '1 2 3 1 3'::text, '1 <-> 3', 'MaxWords=4, MinWords=1');

--Check if headline fragments work
--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg ', to_tsquery('english', 'ocean'), 'MaxFragments=1');

--Check if more than one fragments are displayed
--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg ', to_tsquery('english', 'Coleridge & stuck'), 'MaxFragments=2');

--Fragments when there all query words are not in the document
--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg ', to_tsquery('english', 'ocean & seahorse'), 'MaxFragments=1');

--FragmentDelimiter option
--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english', '
--dg Day after day, day after day,
--dg   We stuck, nor breath nor motion,
--dg As idle as a painted Ship
--dg   Upon a painted Ocean.
--dg Water, water, every where
--dg   And all the boards did shrink;
--dg Water, water, every where,
--dg   Nor any drop to drink.
--dg S. T. Coleridge (1772-1834)
--dg ', to_tsquery('english', 'Coleridge & stuck'), 'MaxFragments=2,FragmentDelimiter=***');

--Fragments with phrase search
--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english',
--dg 'Lorem ipsum urna.  Nullam nullam ullamcorper urna.',
--dg to_tsquery('english','Lorem') && phraseto_tsquery('english','ullamcorper urna'),
--dg 'MaxFragments=100, MaxWords=100, MinWords=1');

-- Edge cases with empty query
--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english',
--dg '', to_tsquery('english', ''));
--dg-skip: function ?(...) does not exist
--dg SELECT ts_headline('english',
--dg 'foo bar', to_tsquery('english', ''));

-- Test for large values of StartSel, StopSel and FragmentDelimiter
--dg-differs: another error: function ?(...) does not exist
--dg SELECT ts_headline('english', 'foo barbar', to_tsquery('english', 'foo'),
--dg   'StartSel=' || repeat('x', 32768));
--dg-differs: another error: function ?(...) does not exist
--dg SELECT ts_headline('english', 'foo barbar', to_tsquery('english', 'foo'),
--dg   'StopSel=' || repeat('x', 32768));
--dg-differs: another error: function ?(...) does not exist
--dg SELECT ts_headline('english', 'foo barbar', to_tsquery('english', 'foo'),
--dg   'FragmentDelimiter=' || repeat('x', 32768));

--Rewrite sub system

CREATE TABLE test_tsquery (txtkeyword TEXT, txtsample TEXT);
\set ECHO none
\copy test_tsquery from stdin
'New York'	new <-> york | big <-> apple | nyc
Moscow	moskva | moscow
'Sanct Peter'	Peterburg | peter | 'Sanct Peterburg'
foo & bar & qq	foo & (bar | qq) & city
1 & (2 <-> 3)	2 <-> 4
5 <-> 6	5 <-> 7
\.
\set ECHO all

--dg-skip: type "?" does not exist
--dg ALTER TABLE test_tsquery ADD COLUMN keyword tsquery;
--dg-cascade
--dg UPDATE test_tsquery SET keyword = to_tsquery('english', txtkeyword);
--dg-skip: type "?" does not exist
--dg ALTER TABLE test_tsquery ADD COLUMN sample tsquery;
--dg-cascade
--dg UPDATE test_tsquery SET sample = to_tsquery('english', txtsample::text);


--dg-cascade
--dg SELECT COUNT(*) FROM test_tsquery WHERE keyword <  'new <-> york';
--dg-cascade
--dg SELECT COUNT(*) FROM test_tsquery WHERE keyword <= 'new <-> york';
--dg-cascade
--dg SELECT COUNT(*) FROM test_tsquery WHERE keyword = 'new <-> york';
--dg-cascade
--dg SELECT COUNT(*) FROM test_tsquery WHERE keyword >= 'new <-> york';
--dg-cascade
--dg SELECT COUNT(*) FROM test_tsquery WHERE keyword >  'new <-> york';

--dg-cascade
--dg CREATE UNIQUE INDEX bt_tsq ON test_tsquery (keyword);

SET enable_seqscan=OFF;

--dg-cascade
--dg SELECT COUNT(*) FROM test_tsquery WHERE keyword <  'new <-> york';
--dg-cascade
--dg SELECT COUNT(*) FROM test_tsquery WHERE keyword <= 'new <-> york';
--dg-cascade
--dg SELECT COUNT(*) FROM test_tsquery WHERE keyword = 'new <-> york';
--dg-cascade
--dg SELECT COUNT(*) FROM test_tsquery WHERE keyword >= 'new <-> york';
--dg-cascade
--dg SELECT COUNT(*) FROM test_tsquery WHERE keyword >  'new <-> york';

RESET enable_seqscan;

--dg-skip: type "?" does not exist
--dg SELECT ts_rewrite('foo & bar & qq & new & york',  'new & york'::tsquery, 'big & apple | nyc | new & york & city');
--dg-skip: function ?(...) does not exist
--dg SELECT ts_rewrite(ts_rewrite('new & !york ', 'york', '!jersey'),
--dg                   'jersey', 'mexico');

--dg-cascade
--dg SELECT ts_rewrite('moscow', 'SELECT keyword, sample FROM test_tsquery'::text );
--dg-cascade
--dg SELECT ts_rewrite('moscow & hotel', 'SELECT keyword, sample FROM test_tsquery'::text );
--dg-cascade
--dg SELECT ts_rewrite('bar & qq & foo & (new <-> york)', 'SELECT keyword, sample FROM test_tsquery'::text );

--dg-cascade
--dg SELECT ts_rewrite( 'moscow', 'SELECT keyword, sample FROM test_tsquery');
--dg-cascade
--dg SELECT ts_rewrite( 'moscow & hotel', 'SELECT keyword, sample FROM test_tsquery');
--dg-cascade
--dg SELECT ts_rewrite( 'bar & qq & foo & (new <-> york)', 'SELECT keyword, sample FROM test_tsquery');

--dg-cascade
--dg SELECT ts_rewrite('1 & (2 <-> 3)', 'SELECT keyword, sample FROM test_tsquery'::text );
--dg-cascade
--dg SELECT ts_rewrite('1 & (2 <2> 3)', 'SELECT keyword, sample FROM test_tsquery'::text );
--dg-cascade
--dg SELECT ts_rewrite('5 <-> (1 & (2 <-> 3))', 'SELECT keyword, sample FROM test_tsquery'::text );
--dg-cascade
--dg SELECT ts_rewrite('5 <-> (6 | 8)', 'SELECT keyword, sample FROM test_tsquery'::text );

-- Check empty substitution
--dg-skip: function ?(...) does not exist
--dg SELECT ts_rewrite(to_tsquery('5 & (6 | 5)'), to_tsquery('5'), to_tsquery(''));
--dg-skip: function ?(...) does not exist
--dg SELECT ts_rewrite(to_tsquery('!5'), to_tsquery('5'), to_tsquery(''));

--dg-cascade
--dg SELECT keyword FROM test_tsquery WHERE keyword @> 'new';
--dg-cascade
--dg SELECT keyword FROM test_tsquery WHERE keyword @> 'moscow';
--dg-cascade
--dg SELECT keyword FROM test_tsquery WHERE keyword <@ 'new';
--dg-cascade
--dg SELECT keyword FROM test_tsquery WHERE keyword <@ 'moscow';
--dg-cascade
--dg SELECT ts_rewrite( query, 'SELECT keyword, sample FROM test_tsquery' ) FROM to_tsquery('english', 'moscow') AS query;
--dg-cascade
--dg SELECT ts_rewrite( query, 'SELECT keyword, sample FROM test_tsquery' ) FROM to_tsquery('english', 'moscow & hotel') AS query;
--dg-cascade
--dg SELECT ts_rewrite( query, 'SELECT keyword, sample FROM test_tsquery' ) FROM to_tsquery('english', 'bar & qq & foo & (new <-> york)') AS query;
--dg-cascade
--dg SELECT ts_rewrite( query, 'SELECT keyword, sample FROM test_tsquery' ) FROM to_tsquery('english', 'moscow') AS query;
--dg-cascade
--dg SELECT ts_rewrite( query, 'SELECT keyword, sample FROM test_tsquery' ) FROM to_tsquery('english', 'moscow & hotel') AS query;
--dg-cascade
--dg SELECT ts_rewrite( query, 'SELECT keyword, sample FROM test_tsquery' ) FROM to_tsquery('english', 'bar & qq & foo & (new <-> york)') AS query;

--dg-skip: access method "?" is not supported
--dg CREATE INDEX qq ON test_tsquery USING gist (keyword tsquery_ops);
SET enable_seqscan=OFF;

--dg-cascade
--dg SELECT keyword FROM test_tsquery WHERE keyword @> 'new';
--dg-cascade
--dg SELECT keyword FROM test_tsquery WHERE keyword @> 'moscow';
--dg-cascade
--dg SELECT keyword FROM test_tsquery WHERE keyword <@ 'new';
--dg-cascade
--dg SELECT keyword FROM test_tsquery WHERE keyword <@ 'moscow';
--dg-cascade
--dg SELECT ts_rewrite( query, 'SELECT keyword, sample FROM test_tsquery' ) FROM to_tsquery('english', 'moscow') AS query;
--dg-cascade
--dg SELECT ts_rewrite( query, 'SELECT keyword, sample FROM test_tsquery' ) FROM to_tsquery('english', 'moscow & hotel') AS query;
--dg-cascade
--dg SELECT ts_rewrite( query, 'SELECT keyword, sample FROM test_tsquery' ) FROM to_tsquery('english', 'bar & qq & foo & (new <-> york)') AS query;
--dg-cascade
--dg SELECT ts_rewrite( query, 'SELECT keyword, sample FROM test_tsquery' ) FROM to_tsquery('english', 'moscow') AS query;
--dg-cascade
--dg SELECT ts_rewrite( query, 'SELECT keyword, sample FROM test_tsquery' ) FROM to_tsquery('english', 'moscow & hotel') AS query;
--dg-cascade
--dg SELECT ts_rewrite( query, 'SELECT keyword, sample FROM test_tsquery' ) FROM to_tsquery('english', 'bar & qq & foo & (new <-> york)') AS query;

--dg-skip: function ?(...) does not exist
--dg SELECT ts_rewrite(tsquery_phrase('foo', 'foo'), 'foo', 'bar | baz');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsvector('foo bar') @@
--dg   ts_rewrite(tsquery_phrase('foo', 'foo'), 'foo', 'bar | baz');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsvector('bar baz') @@
--dg   ts_rewrite(tsquery_phrase('foo', 'foo'), 'foo', 'bar | baz');

RESET enable_seqscan;

--test GUC
--dg-skip: unrecognized configuration parameter "?"
--dg SET default_text_search_config=simple;

--dg-cascade
--dg SELECT to_tsvector('SKIES My booKs');
--dg-cascade
--dg SELECT plainto_tsquery('SKIES My booKs');
--dg-cascade
--dg SELECT to_tsquery('SKIES & My | booKs');

--dg-skip: unrecognized configuration parameter "?"
--dg SET default_text_search_config=english;

--dg-skip: function ?(...) does not exist
--dg SELECT to_tsvector('SKIES My booKs');
--dg-skip: function ?(...) does not exist
--dg SELECT plainto_tsquery('SKIES My booKs');
--dg-skip: function ?(...) does not exist
--dg SELECT to_tsquery('SKIES & My | booKs');

--trigger
--dg-cascade
--dg CREATE TRIGGER tsvectorupdate
--dg BEFORE UPDATE OR INSERT ON test_tsvector
--dg FOR EACH ROW EXECUTE PROCEDURE tsvector_update_trigger(a, 'pg_catalog.english', t);

--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ to_tsquery('345&qwerty');
--dg-cascade
--dg INSERT INTO test_tsvector (t) VALUES ('345 qwerty');
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ to_tsquery('345&qwerty');
--dg-cascade
--dg UPDATE test_tsvector SET t = null WHERE t = '345 qwerty';
--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ to_tsquery('345&qwerty');

--dg-cascade
--dg INSERT INTO test_tsvector (t) VALUES ('345 qwerty');

--dg-cascade
--dg SELECT count(*) FROM test_tsvector WHERE a @@ to_tsquery('345&qwerty');

-- Test inlining of immutable constant functions

-- to_tsquery(text) is not immutable, so it won't be inlined
--dg-skip: function ?(...) does not exist
--dg explain (costs off)
--dg select * from test_tsquery, to_tsquery('new') q where txtsample @@ q;

-- to_tsquery(regconfig, text) is an immutable function.
-- That allows us to get rid of using function scan and join at all.
--dg-skip: function ?(...) does not exist
--dg explain (costs off)
--dg select * from test_tsquery, to_tsquery('english', 'new') q where txtsample @@ q;

-- test finding items in GIN's pending list
--dg-skip: type "?" does not exist
--dg create temp table pendtest (ts tsvector);
--dg-cascade
--dg create index pendtest_idx on pendtest using gin(ts);
--dg-cascade
--dg insert into pendtest values (to_tsvector('Lore ipsam'));
--dg-cascade
--dg insert into pendtest values (to_tsvector('Lore ipsum'));
--dg-cascade
--dg select * from pendtest where 'ipsu:*'::tsquery @@ ts;
--dg-cascade
--dg select * from pendtest where 'ipsa:*'::tsquery @@ ts;
--dg-cascade
--dg select * from pendtest where 'ips:*'::tsquery @@ ts;
--dg-cascade
--dg select * from pendtest where 'ipt:*'::tsquery @@ ts;
--dg-cascade
--dg select * from pendtest where 'ipi:*'::tsquery @@ ts;

--check OP_PHRASE on index
--dg-skip: type "?" does not exist
--dg create temp table phrase_index_test(fts tsvector);
--dg-cascade
--dg insert into phrase_index_test values ('A fat cat has just eaten a rat.');
--dg-cascade
--dg insert into phrase_index_test values (to_tsvector('english', 'A fat cat has just eaten a rat.'));
--dg-cascade
--dg create index phrase_index_test_idx on phrase_index_test using gin(fts);
set enable_seqscan = off;
--dg-cascade
--dg select * from phrase_index_test where fts @@ phraseto_tsquery('english', 'fat cat');
set enable_seqscan = on;

-- test websearch_to_tsquery function
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'I have a fat:*ABCD cat');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'orange:**AABBCCDD');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'fat:A!cat:B|rat:C<');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'fat:A : cat:B');

--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'fat*rat');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'fat-rat');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'fat_rat');

-- weights are completely ignored
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'abc : def');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'abc:def');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'a:::b');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'abc:d');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', ':');

-- these operators are ignored
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'abc & def');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'abc | def');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'abc <-> def');

-- parens are ignored, too
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'abc (pg or class)');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', '(foo bar) or (ding dong)');

-- NOT is ignored in quotes
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', 'My brand new smartphone');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', 'My brand "new smartphone"');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', 'My brand "new -smartphone"');

-- test OR operator
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'cat or rat');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'cat OR rat');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'cat "OR" rat');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'cat OR');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'OR rat');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', '"fat cat OR rat"');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'fat (cat OR rat');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'or OR or');

-- OR is an operator here ...
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', '"fat cat"or"fat rat"');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'fat or(rat');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'fat or)rat');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'fat or&rat');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'fat or|rat');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'fat or!rat');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'fat or<rat');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'fat or>rat');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'fat or ');

-- ... but not here
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'abc orange');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'abc OR1234');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'abc or-abc');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('simple', 'abc OR_abc');

-- test quotes
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', '"pg_class pg');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', 'pg_class pg"');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', '"pg_class pg"');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', '"pg_class : pg"');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', 'abc "pg_class pg"');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', '"pg_class pg" def');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', 'abc "pg pg_class pg" def');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', ' or "pg pg_class pg" or ');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', '""pg pg_class pg""');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', 'abc """"" def');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', 'cat -"fat rat"');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', 'cat -"fat rat" cheese');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', 'abc "def -"');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', 'abc "def :"');

--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', '"A fat cat" has just eaten a -rat.');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', '"A fat cat" has just eaten OR !rat.');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', '"A fat cat" has just (+eaten OR -rat)');

--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', 'this is ----fine');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', '(()) )))) this ||| is && -fine, "dear friend" OR good');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', 'an old <-> cat " is fine &&& too');

--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', '"A the" OR just on');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('english', '"a fat cat" ate a rat');

--dg-skip: function ?(...) does not exist
--dg select to_tsvector('english', 'A fat cat ate a rat') @@
--dg 	websearch_to_tsquery('english', '"a fat cat" ate a rat');

--dg-skip: function ?(...) does not exist
--dg select to_tsvector('english', 'A fat grey cat ate a rat') @@
--dg 	websearch_to_tsquery('english', '"a fat cat" ate a rat');

-- cases handled by gettoken_tsvector()
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('''');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('''abc''''def''');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('\abc');
--dg-skip: function ?(...) does not exist
--dg select websearch_to_tsquery('\');
