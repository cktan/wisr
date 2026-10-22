--
-- WINDOW FUNCTIONS
--

CREATE TEMPORARY TABLE empsalary (
    depname varchar,
    empno bigint,
    salary int,
    enroll_date date
);

INSERT INTO empsalary VALUES
('develop', 10, 5200, '2007-08-01'),
('sales', 1, 5000, '2006-10-01'),
('personnel', 5, 3500, '2007-12-10'),
('sales', 4, 4800, '2007-08-08'),
('personnel', 2, 3900, '2006-12-23'),
('develop', 7, 4200, '2008-01-01'),
('develop', 9, 4500, '2008-01-01'),
('sales', 3, 4800, '2007-08-01'),
('develop', 8, 6000, '2006-10-01'),
('develop', 11, 5200, '2007-08-15');

SELECT depname, empno, salary, sum(salary) OVER (PARTITION BY depname) FROM empsalary ORDER BY depname, salary;

SELECT depname, empno, salary, rank() OVER (PARTITION BY depname ORDER BY salary) FROM empsalary;

-- with GROUP BY
SELECT four, ten, SUM(SUM(four)) OVER (PARTITION BY four), AVG(ten) FROM tenk1
GROUP BY four, ten ORDER BY four, ten;

SELECT depname, empno, salary, sum(salary) OVER w FROM empsalary WINDOW w AS (PARTITION BY depname);

SELECT depname, empno, salary, rank() OVER w FROM empsalary WINDOW w AS (PARTITION BY depname ORDER BY salary) ORDER BY rank() OVER w;

-- empty window specification
SELECT COUNT(*) OVER () FROM tenk1 WHERE unique2 < 10;

SELECT COUNT(*) OVER w FROM tenk1 WHERE unique2 < 10 WINDOW w AS ();

-- no window operation
SELECT four FROM tenk1 WHERE FALSE WINDOW w AS (PARTITION BY ten);

-- cumulative aggregate
SELECT sum(four) OVER (PARTITION BY ten ORDER BY unique2) AS sum_1, ten, four FROM tenk1 WHERE unique2 < 10;

SELECT row_number() OVER (ORDER BY unique2) FROM tenk1 WHERE unique2 < 10;

SELECT rank() OVER (PARTITION BY four ORDER BY ten) AS rank_1, ten, four FROM tenk1 WHERE unique2 < 10;

SELECT dense_rank() OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT percent_rank() OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT cume_dist() OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT ntile(3) OVER (ORDER BY ten, four), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT ntile(NULL) OVER (ORDER BY ten, four), ten, four FROM tenk1 LIMIT 2;

SELECT lag(ten) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT lag(ten, four) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT lag(ten, four, 0) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;
SELECT lag(ten, four, 0.7) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10 ORDER BY four, ten;

SELECT lead(ten) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT lead(ten * 2, 1) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT lead(ten * 2, 1, -1) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;
SELECT lead(ten * 2, 1, -1.4) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10 ORDER BY four, ten;

SELECT first_value(ten) OVER (PARTITION BY four ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

-- last_value returns the last row of the frame, which is CURRENT ROW in ORDER BY window.
SELECT last_value(four) OVER (ORDER BY ten), ten, four FROM tenk1 WHERE unique2 < 10;

SELECT last_value(ten) OVER (PARTITION BY four), ten, four FROM
	(SELECT * FROM tenk1 WHERE unique2 < 10 ORDER BY four, ten)s
	ORDER BY four, ten;

SELECT nth_value(ten, four + 1) OVER (PARTITION BY four), ten, four
	FROM (SELECT * FROM tenk1 WHERE unique2 < 10 ORDER BY four, ten)s;

SELECT ten, two, sum(hundred) AS gsum, sum(sum(hundred)) OVER (PARTITION BY two ORDER BY ten) AS wsum
FROM tenk1 GROUP BY ten, two;

SELECT count(*) OVER (PARTITION BY four), four FROM (SELECT * FROM tenk1 WHERE two = 1)s WHERE unique2 < 10;

SELECT (count(*) OVER (PARTITION BY four ORDER BY ten) +
  sum(hundred) OVER (PARTITION BY four ORDER BY ten))::varchar AS cntsum
  FROM tenk1 WHERE unique2 < 10;

-- opexpr with different windows evaluation.
SELECT * FROM(
  SELECT count(*) OVER (PARTITION BY four ORDER BY ten) +
    sum(hundred) OVER (PARTITION BY two ORDER BY ten) AS total,
    count(*) OVER (PARTITION BY four ORDER BY ten) AS fourcount,
    sum(hundred) OVER (PARTITION BY two ORDER BY ten) AS twosum
    FROM tenk1
)sub
WHERE total <> fourcount + twosum;

SELECT avg(four) OVER (PARTITION BY four ORDER BY thousand / 100) FROM tenk1 WHERE unique2 < 10;

SELECT ten, two, sum(hundred) AS gsum, sum(sum(hundred)) OVER win AS wsum
FROM tenk1 GROUP BY ten, two WINDOW win AS (PARTITION BY two ORDER BY ten);

-- more than one window with GROUP BY
SELECT sum(salary),
	row_number() OVER (ORDER BY depname),
	sum(sum(salary)) OVER (ORDER BY depname DESC)
FROM empsalary GROUP BY depname;

-- identical windows with different names
SELECT sum(salary) OVER w1, count(*) OVER w2
FROM empsalary WINDOW w1 AS (ORDER BY salary), w2 AS (ORDER BY salary);

-- subplan
SELECT lead(ten, (SELECT two FROM tenk1 WHERE s.unique2 = unique2)) OVER (PARTITION BY four ORDER BY ten)
FROM tenk1 s WHERE unique2 < 10;

-- empty table
SELECT count(*) OVER (PARTITION BY four) FROM (SELECT * FROM tenk1 WHERE FALSE)s;

-- mixture of agg/wfunc in the same window
SELECT sum(salary) OVER w, rank() OVER w FROM empsalary WINDOW w AS (PARTITION BY depname ORDER BY salary DESC);

-- strict aggs
SELECT empno, depname, salary, bonus, depadj, MIN(bonus) OVER (ORDER BY empno), MAX(depadj) OVER () FROM(
	SELECT *,
		CASE WHEN enroll_date < '2008-01-01' THEN 2008 - extract(YEAR FROM enroll_date) END * 500 AS bonus,
		CASE WHEN
			AVG(salary) OVER (PARTITION BY depname) < salary
		THEN 200 END AS depadj FROM empsalary
)s;

-- window function over ungrouped agg over empty row set (bug before 9.1)
SELECT SUM(COUNT(f1)) OVER () FROM int4_tbl WHERE f1=42;

-- window function with ORDER BY an expression involving aggregates (9.1 bug)
select ten,
  sum(unique1) + sum(unique2) as res,
  rank() over (order by sum(unique1) + sum(unique2)) as rank
from tenk1
group by ten order by ten;

-- window and aggregate with GROUP BY expression (9.2 bug)
explain (costs off)
select first_value(max(x)) over (), y
  from (select unique1 as x, ten+four as y from tenk1) ss
  group by y;

-- window functions returning pass-by-ref values from different rows
select x, lag(x, 1) over (order by x), lead(x, 3) over (order by x)
from (select x::numeric as x from generate_series(1,10) x);

-- test non-default frame specifications
SELECT four, ten,
	sum(ten) over (partition by four order by ten),
	last_value(ten) over (partition by four order by ten)
FROM (select distinct ten, four from tenk1) ss;

SELECT four, ten,
	sum(ten) over (partition by four order by ten range between unbounded preceding and current row),
	last_value(ten) over (partition by four order by ten range between unbounded preceding and current row)
FROM (select distinct ten, four from tenk1) ss;

SELECT four, ten,
	sum(ten) over (partition by four order by ten range between unbounded preceding and unbounded following),
	last_value(ten) over (partition by four order by ten range between unbounded preceding and unbounded following)
FROM (select distinct ten, four from tenk1) ss;

SELECT four, ten/4 as two,
	sum(ten/4) over (partition by four order by ten/4 range between unbounded preceding and current row),
	last_value(ten/4) over (partition by four order by ten/4 range between unbounded preceding and current row)
FROM (select distinct ten, four from tenk1) ss;

SELECT four, ten/4 as two,
	sum(ten/4) over (partition by four order by ten/4 rows between unbounded preceding and current row),
	last_value(ten/4) over (partition by four order by ten/4 rows between unbounded preceding and current row)
FROM (select distinct ten, four from tenk1) ss;

SELECT sum(unique1) over (order by four range between current row and unbounded following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between current row and unbounded following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between 2 preceding and 2 following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between 2 preceding and 2 following exclude no others),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between 2 preceding and 2 following exclude current row),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between 2 preceding and 2 following exclude group),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between 2 preceding and 2 following exclude ties),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT first_value(unique1) over (ORDER BY four rows between current row and 2 following exclude current row),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT first_value(unique1) over (ORDER BY four rows between current row and 2 following exclude group),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT first_value(unique1) over (ORDER BY four rows between current row and 2 following exclude ties),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT last_value(unique1) over (ORDER BY four rows between current row and 2 following exclude current row),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT last_value(unique1) over (ORDER BY four rows between current row and 2 following exclude group),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT last_value(unique1) over (ORDER BY four rows between current row and 2 following exclude ties),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between 2 preceding and 1 preceding),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between 1 following and 3 following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between unbounded preceding and 1 following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (w range between current row and unbounded following),
	unique1, four
FROM tenk1 WHERE unique1 < 10 WINDOW w AS (order by four);

SELECT sum(unique1) over (w range between unbounded preceding and current row exclude current row),
	unique1, four
FROM tenk1 WHERE unique1 < 10 WINDOW w AS (order by four);

SELECT sum(unique1) over (w range between unbounded preceding and current row exclude group),
	unique1, four
FROM tenk1 WHERE unique1 < 10 WINDOW w AS (order by four);

SELECT sum(unique1) over (w range between unbounded preceding and current row exclude ties),
	unique1, four
FROM tenk1 WHERE unique1 < 10 WINDOW w AS (order by four);

SELECT first_value(unique1) over w,
	nth_value(unique1, 2) over w AS nth_2,
	last_value(unique1) over w, unique1, four
FROM tenk1 WHERE unique1 < 10
WINDOW w AS (order by four range between current row and unbounded following);

SELECT sum(unique1) over
	(order by unique1
	 rows (SELECT unique1 FROM tenk1 ORDER BY unique1 LIMIT 1) + 1 PRECEDING),
	unique1
FROM tenk1 WHERE unique1 < 10;

CREATE TEMP VIEW v_window AS
	SELECT i, sum(i) over (order by i rows between 1 preceding and 1 following) as sum_rows
	FROM generate_series(1, 10) i;

SELECT * FROM v_window;

SELECT pg_get_viewdef('v_window');

CREATE OR REPLACE TEMP VIEW v_window AS
	SELECT i, sum(i) over (order by i rows between 1 preceding and 1 following
	exclude current row) as sum_rows FROM generate_series(1, 10) i;

SELECT * FROM v_window;

SELECT pg_get_viewdef('v_window');

CREATE OR REPLACE TEMP VIEW v_window AS
	SELECT i, sum(i) over (order by i rows between 1 preceding and 1 following
	exclude group) as sum_rows FROM generate_series(1, 10) i;

SELECT * FROM v_window;

SELECT pg_get_viewdef('v_window');

CREATE OR REPLACE TEMP VIEW v_window AS
	SELECT i, sum(i) over (order by i rows between 1 preceding and 1 following
	exclude ties) as sum_rows FROM generate_series(1, 10) i;

SELECT * FROM v_window;

SELECT pg_get_viewdef('v_window');

CREATE OR REPLACE TEMP VIEW v_window AS
	SELECT i, sum(i) over (order by i rows between 1 preceding and 1 following
	exclude no others) as sum_rows FROM generate_series(1, 10) i;

SELECT * FROM v_window;

SELECT pg_get_viewdef('v_window');

CREATE OR REPLACE TEMP VIEW v_window AS
	SELECT i, sum(i) over (order by i groups between 1 preceding and 1 following) as sum_rows FROM generate_series(1, 10) i;

SELECT * FROM v_window;

SELECT pg_get_viewdef('v_window');

DROP VIEW v_window;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg CREATE TEMP VIEW v_window AS
--dg 	SELECT i, min(i) over (order by i range between '1 day' preceding and '10 days' following) as min_i
--dg   FROM generate_series(now(), now()+'100 days'::interval, '1 hour') i;

--dg-cascade
--dg SELECT pg_get_viewdef('v_window');

-- test overflow frame specifications
SELECT sum(unique1) over (rows between current row and 9223372036854775807 following exclude current row),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between 9223372036854775807 following and 1 following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT last_value(unique1) over (ORDER BY four rows between current row and 9223372036854775807 following exclude current row),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

-- These test GROUPS mode with an offset large enough to cause overflow when
-- added to currentgroup.  Although the overflow doesn't produce visibly wrong
-- results (due to the incremental nature of group pointer advancement), we
-- still need to protect against it as signed integer overflow is undefined
-- behavior in C.
SELECT sum(unique1) over (ORDER BY four groups between current row and 9223372036854775807 following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (ORDER BY four groups between 9223372036854775807 following and unbounded following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

-- RANGE offset PRECEDING/FOLLOWING tests

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg SELECT sum(unique1) over (order by four range between 2::int8 preceding and 1::int2 preceding),
--dg 	unique1, four
--dg FROM tenk1 WHERE unique1 < 10;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg SELECT sum(unique1) over (order by four desc range between 2::int8 preceding and 1::int2 preceding),
--dg 	unique1, four
--dg FROM tenk1 WHERE unique1 < 10;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg SELECT sum(unique1) over (order by four range between 2::int8 preceding and 1::int2 preceding exclude no others),
--dg 	unique1, four
--dg FROM tenk1 WHERE unique1 < 10;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg SELECT sum(unique1) over (order by four range between 2::int8 preceding and 1::int2 preceding exclude current row),
--dg 	unique1, four
--dg FROM tenk1 WHERE unique1 < 10;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg SELECT sum(unique1) over (order by four range between 2::int8 preceding and 1::int2 preceding exclude group),
--dg 	unique1, four
--dg FROM tenk1 WHERE unique1 < 10;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg SELECT sum(unique1) over (order by four range between 2::int8 preceding and 1::int2 preceding exclude ties),
--dg 	unique1, four
--dg FROM tenk1 WHERE unique1 < 10;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg SELECT sum(unique1) over (order by four range between 2::int8 preceding and 6::int2 following exclude ties),
--dg 	unique1, four
--dg FROM tenk1 WHERE unique1 < 10;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg SELECT sum(unique1) over (order by four range between 2::int8 preceding and 6::int2 following exclude group),
--dg 	unique1, four
--dg FROM tenk1 WHERE unique1 < 10;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg SELECT sum(unique1) over (partition by four order by unique1 range between 5::int8 preceding and 6::int2 following),
--dg 	unique1, four
--dg FROM tenk1 WHERE unique1 < 10;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg SELECT sum(unique1) over (partition by four order by unique1 range between 5::int8 preceding and 6::int2 following
--dg 	exclude current row),unique1, four
--dg FROM tenk1 WHERE unique1 < 10;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select sum(salary) over (order by enroll_date range between '1 year'::interval preceding and '1 year'::interval following),
--dg 	salary, enroll_date from empsalary;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select sum(salary) over (order by enroll_date desc range between '1 year'::interval preceding and '1 year'::interval following),
--dg 	salary, enroll_date from empsalary;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select sum(salary) over (order by enroll_date desc range between '1 year'::interval following and '1 year'::interval following),
--dg 	salary, enroll_date from empsalary;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select sum(salary) over (order by enroll_date range between '1 year'::interval preceding and '1 year'::interval following
--dg 	exclude current row), salary, enroll_date from empsalary;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select sum(salary) over (order by enroll_date range between '1 year'::interval preceding and '1 year'::interval following
--dg 	exclude group), salary, enroll_date from empsalary;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select sum(salary) over (order by enroll_date range between '1 year'::interval preceding and '1 year'::interval following
--dg 	exclude ties), salary, enroll_date from empsalary;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select first_value(salary) over(order by salary range between 1000 preceding and 1000 following),
--dg 	lead(salary) over(order by salary range between 1000 preceding and 1000 following),
--dg 	nth_value(salary, 1) over(order by salary range between 1000 preceding and 1000 following),
--dg 	salary from empsalary;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select last_value(salary) over(order by salary range between 1000 preceding and 1000 following),
--dg 	lag(salary) over(order by salary range between 1000 preceding and 1000 following),
--dg 	salary from empsalary;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select first_value(salary) over(order by salary range between 1000 following and 3000 following
--dg 	exclude current row),
--dg 	lead(salary) over(order by salary range between 1000 following and 3000 following exclude ties),
--dg 	nth_value(salary, 1) over(order by salary range between 1000 following and 3000 following
--dg 	exclude ties),
--dg 	salary from empsalary;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select last_value(salary) over(order by salary range between 1000 following and 3000 following
--dg 	exclude group),
--dg 	lag(salary) over(order by salary range between 1000 following and 3000 following exclude group),
--dg 	salary from empsalary;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select first_value(salary) over(order by enroll_date range between unbounded preceding and '1 year'::interval following
--dg 	exclude ties),
--dg 	last_value(salary) over(order by enroll_date range between unbounded preceding and '1 year'::interval following),
--dg 	salary, enroll_date from empsalary;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select first_value(salary) over(order by enroll_date range between unbounded preceding and '1 year'::interval following
--dg 	exclude ties),
--dg 	last_value(salary) over(order by enroll_date range between unbounded preceding and '1 year'::interval following
--dg 	exclude ties),
--dg 	salary, enroll_date from empsalary;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select first_value(salary) over(order by enroll_date range between unbounded preceding and '1 year'::interval following
--dg 	exclude group),
--dg 	last_value(salary) over(order by enroll_date range between unbounded preceding and '1 year'::interval following
--dg 	exclude group),
--dg 	salary, enroll_date from empsalary;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select first_value(salary) over(order by enroll_date range between unbounded preceding and '1 year'::interval following
--dg 	exclude current row),
--dg 	last_value(salary) over(order by enroll_date range between unbounded preceding and '1 year'::interval following
--dg 	exclude current row),
--dg 	salary, enroll_date from empsalary;

-- RANGE offset PRECEDING/FOLLOWING with null values
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select x, y,
--dg        first_value(y) over w,
--dg        last_value(y) over w
--dg from
--dg   (select x, x as y from generate_series(1,5) as x
--dg    union all select null, 42
--dg    union all select null, 43) ss
--dg window w as
--dg   (order by x asc nulls first range between 2 preceding and 2 following);

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select x, y,
--dg        first_value(y) over w,
--dg        last_value(y) over w
--dg from
--dg   (select x, x as y from generate_series(1,5) as x
--dg    union all select null, 42
--dg    union all select null, 43) ss
--dg window w as
--dg   (order by x asc nulls last range between 2 preceding and 2 following);

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select x, y,
--dg        first_value(y) over w,
--dg        last_value(y) over w
--dg from
--dg   (select x, x as y from generate_series(1,5) as x
--dg    union all select null, 42
--dg    union all select null, 43) ss
--dg window w as
--dg   (order by x desc nulls first range between 2 preceding and 2 following);

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select x, y,
--dg        first_value(y) over w,
--dg        last_value(y) over w
--dg from
--dg   (select x, x as y from generate_series(1,5) as x
--dg    union all select null, 42
--dg    union all select null, 43) ss
--dg window w as
--dg   (order by x desc nulls last range between 2 preceding and 2 following);

-- There is a syntactic ambiguity in the SQL standard.  Since
-- UNBOUNDED is a non-reserved word, it could be the name of a
-- function parameter and be used as an expression.  There is a
-- grammar hack to resolve such cases as the keyword.  The following
-- tests record this behavior.

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION unbounded_syntax_test1a(x int) RETURNS TABLE (a int, b int, c int)
--dg LANGUAGE SQL
--dg BEGIN ATOMIC
--dg   SELECT sum(unique1) over (rows between x preceding and x following),
--dg          unique1, four
--dg   FROM tenk1 WHERE unique1 < 10;
--dg END;

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION unbounded_syntax_test1b(x int) RETURNS TABLE (a int, b int, c int)
--dg LANGUAGE SQL
--dg AS $$
--dg   SELECT sum(unique1) over (rows between x preceding and x following),
--dg          unique1, four
--dg   FROM tenk1 WHERE unique1 < 10;
--dg $$;

-- These will apply the argument to the window specification inside the function.
--dg-cascade
--dg SELECT * FROM unbounded_syntax_test1a(2);
--dg-cascade
--dg SELECT * FROM unbounded_syntax_test1b(2);

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION unbounded_syntax_test2a(unbounded int) RETURNS TABLE (a int, b int, c int)
--dg LANGUAGE SQL
--dg BEGIN ATOMIC
--dg   SELECT sum(unique1) over (rows between unbounded preceding and unbounded following),
--dg          unique1, four
--dg   FROM tenk1 WHERE unique1 < 10;
--dg END;

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION unbounded_syntax_test2b(unbounded int) RETURNS TABLE (a int, b int, c int)
--dg LANGUAGE SQL
--dg AS $$
--dg   SELECT sum(unique1) over (rows between unbounded preceding and unbounded following),
--dg          unique1, four
--dg   FROM tenk1 WHERE unique1 < 10;
--dg $$;

-- These will not apply the argument but instead treat UNBOUNDED as a keyword.
--dg-cascade
--dg SELECT * FROM unbounded_syntax_test2a(2);
--dg-cascade
--dg SELECT * FROM unbounded_syntax_test2b(2);

--dg-cascade
--dg DROP FUNCTION unbounded_syntax_test1a, unbounded_syntax_test1b,
--dg               unbounded_syntax_test2a, unbounded_syntax_test2b;

-- Other tests with token UNBOUNDED in potentially problematic position
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION unbounded(x int) RETURNS int LANGUAGE SQL IMMUTABLE RETURN x;

SELECT sum(unique1) over (rows between 1 preceding and 1 following),
       unique1, four
FROM tenk1 WHERE unique1 < 10;

--dg-cascade
--dg SELECT sum(unique1) over (rows between unbounded(1) preceding and unbounded(1) following),
--dg        unique1, four
--dg FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (rows between unbounded.x preceding and unbounded.x following),
       unique1, four
FROM tenk1, (values (1)) as unbounded(x) WHERE unique1 < 10;

--dg-cascade
--dg DROP FUNCTION unbounded;

-- Check overflow behavior for various integer sizes

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select x, last_value(x) over (order by x::smallint range between current row and 2147450884 following)
--dg from generate_series(32764, 32766) x;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select x, last_value(x) over (order by x::smallint desc range between current row and 2147450885 following)
--dg from generate_series(-32766, -32764) x;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select x, last_value(x) over (order by x range between current row and 4 following)
--dg from generate_series(2147483644, 2147483646) x;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select x, last_value(x) over (order by x desc range between current row and 5 following)
--dg from generate_series(-2147483646, -2147483644) x;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select x, last_value(x) over (order by x range between current row and 4 following)
--dg from generate_series(9223372036854775804, 9223372036854775806) x;

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select x, last_value(x) over (order by x desc range between current row and 5 following)
--dg from generate_series(-9223372036854775806, -9223372036854775804) x;

-- Test in_range for other numeric datatypes

create temp table numerics(
    id int,
    f_float4 float4,
    f_float8 float8,
    f_numeric numeric
);

--dg-skip: invalid input syntax for type ?
--dg insert into numerics values
--dg (0, '-infinity', '-infinity', '-infinity'),
--dg (1, -3, -3, -3),
--dg (2, -1, -1, -1),
--dg (3, 0, 0, 0),
--dg (4, 1.1, 1.1, 1.1),
--dg (5, 1.12, 1.12, 1.12),
--dg (6, 2, 2, 2),
--dg (7, 100, 100, 100),
--dg (8, 'infinity', 'infinity', 'infinity'),
--dg (9, 'NaN', 'NaN', 'NaN');

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select id, f_float4, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_float4 range between
--dg              1 preceding and 1 following);
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select id, f_float4, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_float4 range between
--dg              1 preceding and 1.1::float4 following);
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select id, f_float4, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_float4 range between
--dg              'inf' preceding and 'inf' following);
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select id, f_float4, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_float4 range between
--dg              'inf' preceding and 'inf' preceding);
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select id, f_float4, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_float4 range between
--dg              'inf' following and 'inf' following);
--dg-cascade
--dg select id, f_float4, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_float4 range between
--dg              1.1 preceding and 'NaN' following);  -- error, NaN disallowed

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select id, f_float8, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_float8 range between
--dg              1 preceding and 1 following);
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select id, f_float8, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_float8 range between
--dg              1 preceding and 1.1::float8 following);
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select id, f_float8, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_float8 range between
--dg              'inf' preceding and 'inf' following);
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select id, f_float8, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_float8 range between
--dg              'inf' preceding and 'inf' preceding);
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select id, f_float8, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_float8 range between
--dg              'inf' following and 'inf' following);
--dg-cascade
--dg select id, f_float8, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_float8 range between
--dg              1.1 preceding and 'NaN' following);  -- error, NaN disallowed

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select id, f_numeric, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_numeric range between
--dg              1 preceding and 1 following);
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select id, f_numeric, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_numeric range between
--dg              1 preceding and 1.1::numeric following);
--dg-differs: another error: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select id, f_numeric, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_numeric range between
--dg              1 preceding and 1.1::float8 following);  -- currently unsupported
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select id, f_numeric, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_numeric range between
--dg              'inf' preceding and 'inf' following);
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select id, f_numeric, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_numeric range between
--dg              'inf' preceding and 'inf' preceding);
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select id, f_numeric, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_numeric range between
--dg              'inf' following and 'inf' following);
--dg-cascade
--dg select id, f_numeric, first_value(id) over w, last_value(id) over w
--dg from numerics
--dg window w as (order by f_numeric range between
--dg              1.1 preceding and 'NaN' following);  -- error, NaN disallowed

-- Test in_range for other datetime datatypes

--dg-skip: type ? is not supported yet
--dg create temp table datetimes(
--dg     id int,
--dg     f_time time,
--dg     f_timetz timetz,
--dg     f_interval interval,
--dg     f_timestamptz timestamptz,
--dg     f_timestamp timestamp
--dg );

--dg-cascade
--dg insert into datetimes values
--dg (0, '10:00', '10:00 BST', '-infinity', '-infinity', '-infinity'),
--dg (1, '11:00', '11:00 BST', '1 year', '2000-10-19 10:23:54+01', '2000-10-19 10:23:54'),
--dg (2, '12:00', '12:00 BST', '2 years', '2001-10-19 10:23:54+01', '2001-10-19 10:23:54'),
--dg (3, '13:00', '13:00 BST', '3 years', '2001-10-19 10:23:54+01', '2001-10-19 10:23:54'),
--dg (4, '14:00', '14:00 BST', '4 years', '2002-10-19 10:23:54+01', '2002-10-19 10:23:54'),
--dg (5, '15:00', '15:00 BST', '5 years', '2003-10-19 10:23:54+01', '2003-10-19 10:23:54'),
--dg (6, '15:00', '15:00 BST', '5 years', '2004-10-19 10:23:54+01', '2004-10-19 10:23:54'),
--dg (7, '17:00', '17:00 BST', '7 years', '2005-10-19 10:23:54+01', '2005-10-19 10:23:54'),
--dg (8, '18:00', '18:00 BST', '8 years', '2006-10-19 10:23:54+01', '2006-10-19 10:23:54'),
--dg (9, '19:00', '19:00 BST', '9 years', '2007-10-19 10:23:54+01', '2007-10-19 10:23:54'),
--dg (10, '20:00', '20:00 BST', '10 years', '2008-10-19 10:23:54+01', '2008-10-19 10:23:54'),
--dg (11, '21:00', '21:00 BST', 'infinity', 'infinity', 'infinity');

--dg-cascade
--dg select id, f_time, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_time range between
--dg              '70 min'::interval preceding and '2 hours'::interval following);

--dg-cascade
--dg select id, f_time, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_time desc range between
--dg              '70 min' preceding and '2 hours' following);

--dg-cascade
--dg select id, f_time, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_time desc range between
--dg              '-70 min' preceding and '2 hours' following);  -- error, negative offset disallowed

--dg-cascade
--dg select id, f_time, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_time range between
--dg              'infinity'::interval preceding and 'infinity'::interval following);

--dg-cascade
--dg select id, f_time, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_time range between
--dg              'infinity'::interval preceding and 'infinity'::interval preceding);

--dg-cascade
--dg select id, f_time, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_time range between
--dg              'infinity'::interval following and 'infinity'::interval following);

--dg-cascade
--dg select id, f_time, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_time range between
--dg              '-infinity'::interval following and
--dg              'infinity'::interval following);  -- error, negative offset disallowed

--dg-cascade
--dg select id, f_timetz, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timetz range between
--dg              '70 min'::interval preceding and '2 hours'::interval following);

--dg-cascade
--dg select id, f_timetz, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timetz desc range between
--dg              '70 min' preceding and '2 hours' following);

--dg-cascade
--dg select id, f_timetz, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timetz desc range between
--dg              '70 min' preceding and '-2 hours' following);  -- error, negative offset disallowed

--dg-cascade
--dg select id, f_timetz, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timetz range between
--dg              'infinity'::interval preceding and 'infinity'::interval following);

--dg-cascade
--dg select id, f_timetz, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timetz range between
--dg              'infinity'::interval preceding and 'infinity'::interval preceding);

--dg-cascade
--dg select id, f_timetz, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timetz range between
--dg              'infinity'::interval following and 'infinity'::interval following);

--dg-cascade
--dg select id, f_timetz, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timetz range between
--dg              'infinity'::interval following and
--dg              '-infinity'::interval following);  -- error, negative offset disallowed

--dg-cascade
--dg select id, f_interval, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_interval range between
--dg              '1 year'::interval preceding and '1 year'::interval following);

--dg-cascade
--dg select id, f_interval, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_interval desc range between
--dg              '1 year' preceding and '1 year' following);

--dg-cascade
--dg select id, f_interval, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_interval desc range between
--dg              '-1 year' preceding and '1 year' following);  -- error, negative offset disallowed

--dg-cascade
--dg select id, f_interval, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_interval range between
--dg              'infinity'::interval preceding and 'infinity'::interval following);

--dg-cascade
--dg select id, f_interval, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_interval range between
--dg              'infinity'::interval preceding and 'infinity'::interval preceding);

--dg-cascade
--dg select id, f_interval, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_interval range between
--dg              'infinity'::interval following and 'infinity'::interval following);

--dg-cascade
--dg select id, f_interval, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_interval range between
--dg              '-infinity'::interval following and
--dg              'infinity'::interval following);  -- error, negative offset disallowed

--dg-cascade
--dg select id, f_timestamptz, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timestamptz range between
--dg              '1 year'::interval preceding and '1 year'::interval following);

--dg-cascade
--dg select id, f_timestamptz, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timestamptz desc range between
--dg              '1 year' preceding and '1 year' following);

--dg-cascade
--dg select id, f_timestamptz, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timestamptz desc range between
--dg              '1 year' preceding and '-1 year' following);  -- error, negative offset disallowed

--dg-cascade
--dg select id, f_timestamptz, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timestamptz range between
--dg              'infinity'::interval preceding and 'infinity'::interval following);

--dg-cascade
--dg select id, f_timestamptz, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timestamptz range between
--dg              'infinity'::interval preceding and 'infinity'::interval preceding);

--dg-cascade
--dg select id, f_timestamptz, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timestamptz range between
--dg              'infinity'::interval following and 'infinity'::interval following);

--dg-cascade
--dg select id, f_timestamptz, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timestamptz range between
--dg              '-infinity'::interval following and
--dg              'infinity'::interval following);  -- error, negative offset disallowed

--dg-cascade
--dg select id, f_timestamp, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timestamp range between
--dg              '1 year'::interval preceding and '1 year'::interval following);

--dg-cascade
--dg select id, f_timestamp, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timestamp desc range between
--dg              '1 year' preceding and '1 year' following);

--dg-cascade
--dg select id, f_timestamp, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timestamp desc range between
--dg              '-1 year' preceding and '1 year' following);  -- error, negative offset disallowed

--dg-cascade
--dg select id, f_timestamp, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timestamp range between
--dg              'infinity'::interval preceding and 'infinity'::interval following);

--dg-cascade
--dg select id, f_timestamp, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timestamp range between
--dg              'infinity'::interval preceding and 'infinity'::interval preceding);

--dg-cascade
--dg select id, f_timestamp, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timestamp range between
--dg              'infinity'::interval following and 'infinity'::interval following);

--dg-cascade
--dg select id, f_timestamp, first_value(id) over w, last_value(id) over w
--dg from datetimes
--dg window w as (order by f_timestamp range between
--dg              '-infinity'::interval following and
--dg              'infinity'::interval following);  -- error, negative offset disallowed

-- RANGE offset PRECEDING/FOLLOWING error cases
select sum(salary) over (order by enroll_date, salary range between '1 year'::interval preceding and '2 years'::interval following
	exclude ties), salary, enroll_date from empsalary;

select sum(salary) over (range between '1 year'::interval preceding and '2 years'::interval following
	exclude ties), salary, enroll_date from empsalary;

--dg-differs: another error: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select sum(salary) over (order by depname range between '1 year'::interval preceding and '2 years'::interval following
--dg 	exclude ties), salary, enroll_date from empsalary;

--dg-differs: another error: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select max(enroll_date) over (order by enroll_date range between 1 preceding and 2 following
--dg 	exclude ties), salary, enroll_date from empsalary;

--dg-differs: another error: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select max(enroll_date) over (order by salary range between -1 preceding and 2 following
--dg 	exclude ties), salary, enroll_date from empsalary;

--dg-differs: another error: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select max(enroll_date) over (order by salary range between 1 preceding and -2 following
--dg 	exclude ties), salary, enroll_date from empsalary;

--dg-differs: another error: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select max(enroll_date) over (order by salary range between '1 year'::interval preceding and '2 years'::interval following
--dg 	exclude ties), salary, enroll_date from empsalary;

--dg-differs: another error: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select max(enroll_date) over (order by enroll_date range between '1 year'::interval preceding and '-2 years'::interval following
--dg 	exclude ties), salary, enroll_date from empsalary;

-- GROUPS tests

SELECT sum(unique1) over (order by four groups between unbounded preceding and current row),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between unbounded preceding and unbounded following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between current row and unbounded following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between 1 preceding and unbounded following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between 1 following and unbounded following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between unbounded preceding and 2 following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between 2 preceding and 1 preceding),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between 2 preceding and 1 following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between 0 preceding and 0 following),
	unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between 2 preceding and 1 following
	exclude current row), unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between 2 preceding and 1 following
	exclude group), unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (order by four groups between 2 preceding and 1 following
	exclude ties), unique1, four
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (partition by ten
	order by four groups between 0 preceding and 0 following),unique1, four, ten
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (partition by ten
	order by four groups between 0 preceding and 0 following exclude current row), unique1, four, ten
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (partition by ten
	order by four groups between 0 preceding and 0 following exclude group), unique1, four, ten
FROM tenk1 WHERE unique1 < 10;

SELECT sum(unique1) over (partition by ten
	order by four groups between 0 preceding and 0 following exclude ties), unique1, four, ten
FROM tenk1 WHERE unique1 < 10;

select first_value(salary) over(order by enroll_date groups between 1 preceding and 1 following),
	lead(salary) over(order by enroll_date groups between 1 preceding and 1 following),
	nth_value(salary, 1) over(order by enroll_date groups between 1 preceding and 1 following),
	salary, enroll_date from empsalary;

select last_value(salary) over(order by enroll_date groups between 1 preceding and 1 following),
	lag(salary) over(order by enroll_date groups between 1 preceding and 1 following),
	salary, enroll_date from empsalary;

select first_value(salary) over(order by enroll_date groups between 1 following and 3 following
	exclude current row),
	lead(salary) over(order by enroll_date groups between 1 following and 3 following exclude ties),
	nth_value(salary, 1) over(order by enroll_date groups between 1 following and 3 following
	exclude ties),
	salary, enroll_date from empsalary;

select last_value(salary) over(order by enroll_date groups between 1 following and 3 following
	exclude group),
	lag(salary) over(order by enroll_date groups between 1 following and 3 following exclude group),
	salary, enroll_date from empsalary;

-- Show differences in offset interpretation between ROWS, RANGE, and GROUPS
WITH cte (x) AS (
        SELECT * FROM generate_series(1, 35, 2)
)
SELECT x, (sum(x) over w)
FROM cte
WINDOW w AS (ORDER BY x rows between 1 preceding and 1 following);

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg WITH cte (x) AS (
--dg         SELECT * FROM generate_series(1, 35, 2)
--dg )
--dg SELECT x, (sum(x) over w)
--dg FROM cte
--dg WINDOW w AS (ORDER BY x range between 1 preceding and 1 following);

WITH cte (x) AS (
        SELECT * FROM generate_series(1, 35, 2)
)
SELECT x, (sum(x) over w)
FROM cte
WINDOW w AS (ORDER BY x groups between 1 preceding and 1 following);

WITH cte (x) AS (
        select 1 union all select 1 union all select 1 union all
        SELECT * FROM generate_series(5, 49, 2)
)
SELECT x, (sum(x) over w)
FROM cte
WINDOW w AS (ORDER BY x rows between 1 preceding and 1 following);

--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg WITH cte (x) AS (
--dg         select 1 union all select 1 union all select 1 union all
--dg         SELECT * FROM generate_series(5, 49, 2)
--dg )
--dg SELECT x, (sum(x) over w)
--dg FROM cte
--dg WINDOW w AS (ORDER BY x range between 1 preceding and 1 following);

WITH cte (x) AS (
        select 1 union all select 1 union all select 1 union all
        SELECT * FROM generate_series(5, 49, 2)
)
SELECT x, (sum(x) over w)
FROM cte
WINDOW w AS (ORDER BY x groups between 1 preceding and 1 following);

-- with UNION
SELECT count(*) OVER (PARTITION BY four) FROM (SELECT * FROM tenk1 UNION ALL SELECT * FROM tenk2)s LIMIT 0;

-- check some degenerate cases
create temp table t1 (f1 int, f2 int8);
insert into t1 values (1,1),(1,2),(2,2);

select f1, sum(f1) over (partition by f1
                         range between 1 preceding and 1 following)
from t1 where f1 = f2;  -- error, must have order by
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg explain (costs off)
--dg select f1, sum(f1) over (partition by f1 order by f2
--dg                          range between 1 preceding and 1 following)
--dg from t1 where f1 = f2;
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select f1, sum(f1) over (partition by f1 order by f2
--dg                          range between 1 preceding and 1 following)
--dg from t1 where f1 = f2;
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select f1, sum(f1) over (partition by f1, f1 order by f2
--dg                          range between 2 preceding and 1 preceding)
--dg from t1 where f1 = f2;
--dg-skip: relation pg_amproc cannot be opened: digres has no catalog tables
--dg select f1, sum(f1) over (partition by f1, f2 order by f2
--dg                          range between 1 following and 2 following)
--dg from t1 where f1 = f2;

select f1, sum(f1) over (partition by f1
                         groups between 1 preceding and 1 following)
from t1 where f1 = f2;  -- error, must have order by
explain (costs off)
select f1, sum(f1) over (partition by f1 order by f2
                         groups between 1 preceding and 1 following)
from t1 where f1 = f2;
select f1, sum(f1) over (partition by f1 order by f2
                         groups between 1 preceding and 1 following)
from t1 where f1 = f2;
select f1, sum(f1) over (partition by f1, f1 order by f2
                         groups between 2 preceding and 1 preceding)
from t1 where f1 = f2;
select f1, sum(f1) over (partition by f1, f2 order by f2
                         groups between 1 following and 2 following)
from t1 where f1 = f2;

-- ordering by a non-integer constant is allowed
SELECT rank() OVER (ORDER BY length('abc'));

-- can't order by another window function
SELECT rank() OVER (ORDER BY rank() OVER (ORDER BY random()));

-- some other errors
SELECT * FROM empsalary WHERE row_number() OVER (ORDER BY salary) < 10;

SELECT * FROM empsalary INNER JOIN tenk1 ON row_number() OVER (ORDER BY salary) < 10;

SELECT rank() OVER (ORDER BY 1), count(*) FROM empsalary GROUP BY 1;

SELECT * FROM rank() OVER (ORDER BY random());

DELETE FROM empsalary WHERE (rank() OVER (ORDER BY random())) > 10;

DELETE FROM empsalary RETURNING rank() OVER (ORDER BY random());

SELECT count(*) OVER w FROM tenk1 WINDOW w AS (ORDER BY unique1), w AS (ORDER BY unique1);

SELECT rank() OVER (PARTITION BY four, ORDER BY ten) FROM tenk1;

SELECT count() OVER () FROM tenk1;

SELECT generate_series(1, 100) OVER () FROM empsalary;

SELECT ntile(0) OVER (ORDER BY ten), ten, four FROM tenk1;

SELECT nth_value(four, 0) OVER (ORDER BY ten), ten, four FROM tenk1;

-- filter

SELECT sum(salary), row_number() OVER (ORDER BY depname), sum(
    sum(salary) FILTER (WHERE enroll_date > '2007-01-01')
) FILTER (WHERE depname <> 'sales') OVER (ORDER BY depname DESC) AS "filtered_sum",
    depname
FROM empsalary GROUP BY depname;

--
-- Test SupportRequestOptimizeWindowClause's ability to de-duplicate
-- WindowClauses
--

-- Ensure WindowClause frameOptions are changed so that only a single
-- WindowAgg exists in the plan.
EXPLAIN (COSTS OFF)
SELECT
    empno,
    depname,
    row_number() OVER (PARTITION BY depname ORDER BY enroll_date) rn,
    rank() OVER (PARTITION BY depname ORDER BY enroll_date ROWS BETWEEN
                 UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) rnk,
    dense_rank() OVER (PARTITION BY depname ORDER BY enroll_date RANGE BETWEEN
                       CURRENT ROW AND CURRENT ROW) drnk,
    ntile(10) OVER (PARTITION BY depname ORDER BY enroll_date RANGE BETWEEN
                    CURRENT ROW AND UNBOUNDED FOLLOWING) nt,
    percent_rank() OVER (PARTITION BY depname ORDER BY enroll_date ROWS BETWEEN
                         CURRENT ROW AND UNBOUNDED FOLLOWING) pr,
    cume_dist() OVER (PARTITION BY depname ORDER BY enroll_date RANGE BETWEEN
                      CURRENT ROW AND UNBOUNDED FOLLOWING) cd
FROM empsalary;

-- Ensure WindowFuncs which cannot support their WindowClause's frameOptions
-- being changed are untouched
EXPLAIN (COSTS OFF, VERBOSE)
SELECT
    empno,
    depname,
    row_number() OVER (PARTITION BY depname ORDER BY enroll_date) rn,
    rank() OVER (PARTITION BY depname ORDER BY enroll_date ROWS BETWEEN
                 UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) rnk,
    count(*) OVER (PARTITION BY depname ORDER BY enroll_date RANGE BETWEEN
                   CURRENT ROW AND CURRENT ROW) cnt
FROM empsalary;

-- Ensure the above query gives us the expected results
SELECT
    empno,
    depname,
    row_number() OVER (PARTITION BY depname ORDER BY enroll_date) rn,
    rank() OVER (PARTITION BY depname ORDER BY enroll_date ROWS BETWEEN
                 UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) rnk,
    count(*) OVER (PARTITION BY depname ORDER BY enroll_date RANGE BETWEEN
                   CURRENT ROW AND CURRENT ROW) cnt
FROM empsalary;

-- Test pushdown of quals into a subquery containing window functions

-- pushdown is safe because all PARTITION BY clauses include depname:
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT depname,
          sum(salary) OVER (PARTITION BY depname) depsalary,
          min(salary) OVER (PARTITION BY depname || 'A', depname) depminsalary
   FROM empsalary) emp
WHERE depname = 'sales';

-- pushdown is unsafe because there's a PARTITION BY clause without depname:
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT depname,
          sum(salary) OVER (PARTITION BY enroll_date) enroll_salary,
          min(salary) OVER (PARTITION BY depname) depminsalary
   FROM empsalary) emp
WHERE depname = 'sales';

-- Test window function run conditions are properly pushed down into the
-- WindowAgg
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          row_number() OVER (ORDER BY empno) rn
   FROM empsalary) emp
WHERE rn < 3;

-- The following 3 statements should result the same result.
SELECT * FROM
  (SELECT empno,
          row_number() OVER (ORDER BY empno) rn
   FROM empsalary) emp
WHERE rn < 3;

SELECT * FROM
  (SELECT empno,
          row_number() OVER (ORDER BY empno) rn
   FROM empsalary) emp
WHERE 3 > rn;

SELECT * FROM
  (SELECT empno,
          row_number() OVER (ORDER BY empno) rn
   FROM empsalary) emp
WHERE 2 >= rn;

-- Ensure r <= 3 is pushed down into the run condition of the window agg
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          rank() OVER (ORDER BY salary DESC) r
   FROM empsalary) emp
WHERE r <= 3;

SELECT * FROM
  (SELECT empno,
          salary,
          rank() OVER (ORDER BY salary DESC) r
   FROM empsalary) emp
WHERE r <= 3;

-- Ensure dr = 1 is converted to dr <= 1 to get all rows leading up to dr = 1
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          dense_rank() OVER (ORDER BY salary DESC) dr
   FROM empsalary) emp
WHERE dr = 1;

SELECT * FROM
  (SELECT empno,
          salary,
          dense_rank() OVER (ORDER BY salary DESC) dr
   FROM empsalary) emp
WHERE dr = 1;

-- Check COUNT() and COUNT(*)
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(*) OVER (ORDER BY salary DESC) c
   FROM empsalary) emp
WHERE c <= 3;

SELECT * FROM
  (SELECT empno,
          salary,
          count(*) OVER (ORDER BY salary DESC) c
   FROM empsalary) emp
WHERE c <= 3;

EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(empno) OVER (ORDER BY salary DESC) c
   FROM empsalary) emp
WHERE c <= 3;

SELECT * FROM
  (SELECT empno,
          salary,
          count(empno) OVER (ORDER BY salary DESC) c
   FROM empsalary) emp
WHERE c <= 3;

EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(*) OVER (ORDER BY salary DESC ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) c
   FROM empsalary) emp
WHERE c >= 3;

EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(*) OVER () c
   FROM empsalary) emp
WHERE 11 <= c;

EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(*) OVER (ORDER BY salary DESC) c,
          dense_rank() OVER (ORDER BY salary DESC) dr
   FROM empsalary) emp
WHERE dr = 1;

-- Ensure we get a run condition when there's a PARTITION BY clause
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          depname,
          row_number() OVER (PARTITION BY depname ORDER BY empno) rn
   FROM empsalary) emp
WHERE rn < 3;

-- and ensure we get the correct results from the above plan
SELECT * FROM
  (SELECT empno,
          depname,
          row_number() OVER (PARTITION BY depname ORDER BY empno) rn
   FROM empsalary) emp
WHERE rn < 3;

-- ensure that "unused" subquery columns are not removed when the column only
-- exists in the run condition
EXPLAIN (COSTS OFF)
SELECT empno, depname FROM
  (SELECT empno,
          depname,
          row_number() OVER (PARTITION BY depname ORDER BY empno) rn
   FROM empsalary) emp
WHERE rn < 3;

-- likewise with count(empno) instead of row_number()
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          depname,
          salary,
          count(empno) OVER (PARTITION BY depname ORDER BY salary DESC) c
   FROM empsalary) emp
WHERE c <= 3;

-- and again, check the results are what we expect.
SELECT * FROM
  (SELECT empno,
          depname,
          salary,
          count(empno) OVER (PARTITION BY depname ORDER BY salary DESC) c
   FROM empsalary) emp
WHERE c <= 3;

-- Ensure we get the correct run condition when the window function is both
-- monotonically increasing and decreasing in RANGE mode without an ORDER BY
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          depname,
          salary,
          count(empno) OVER (RANGE BETWEEN CURRENT ROW AND CURRENT ROW) c
   FROM empsalary) emp
WHERE c = 1;

-- As above, but check we detect it's monotonically increasing
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          depname,
          salary,
          count(empno) OVER (RANGE BETWEEN CURRENT ROW AND CURRENT ROW) c
   FROM empsalary) emp
WHERE c <= 3;

-- Ensure that ROWS mode without an ORDER BY doesn't think it's monotonically
-- decreasing, i.e. don't push down the run condition.
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          depname,
          salary,
          count(empno) OVER (ROWS BETWEEN CURRENT ROW AND CURRENT ROW) c
   FROM empsalary) emp
WHERE c > 1;

-- Try another case with a WindowFunc with a byref return type
SELECT * FROM
  (SELECT row_number() OVER (PARTITION BY salary) AS rn,
          lead(depname) OVER (PARTITION BY salary) || ' Department' AS n_dep
   FROM empsalary) emp
WHERE rn < 1;

-- Some more complex cases with multiple window clauses
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT *,
          count(salary) OVER (PARTITION BY depname || '') c1, -- w1
          row_number() OVER (PARTITION BY depname) rn, -- w2
          count(*) OVER (PARTITION BY depname) c2, -- w2
          count(*) OVER (PARTITION BY '' || depname) c3, -- w3
          ntile(2) OVER (PARTITION BY depname) nt -- w2
   FROM empsalary
) e WHERE rn <= 1 AND c1 <= 3 AND nt < 2;

-- Ensure we correctly filter out all of the run conditions from each window
SELECT * FROM
  (SELECT *,
          count(salary) OVER (PARTITION BY depname || '') c1, -- w1
          row_number() OVER (PARTITION BY depname) rn, -- w2
          count(*) OVER (PARTITION BY depname) c2, -- w2
          count(*) OVER (PARTITION BY '' || depname) c3, -- w3
          ntile(2) OVER (PARTITION BY depname) nt -- w2
   FROM empsalary
) e WHERE rn <= 1 AND c1 <= 3 AND nt < 2;

-- Ensure we remove references to reduced outer joins as nulling rels in run
-- conditions
EXPLAIN (COSTS OFF)
SELECT 1 FROM
  (SELECT ntile(e2.salary) OVER (PARTITION BY e1.depname) AS c
   FROM empsalary e1 LEFT JOIN empsalary e2 ON TRUE
   WHERE e1.empno = e2.empno) s
WHERE s.c = 1;

-- Ensure the run condition optimization is used in cases where the WindowFunc
-- has a Var from another query level
EXPLAIN (COSTS OFF)
SELECT 1 FROM
  (SELECT ntile(s1.x) OVER () AS c
   FROM (SELECT (SELECT 1) AS x) AS s1) s
WHERE s.c = 1;

-- Tests to ensure we don't push down the run condition when it's not valid to
-- do so.

-- Ensure we don't push down when the frame options show that the window
-- function is not monotonically increasing
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(*) OVER (ORDER BY salary DESC ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) c
   FROM empsalary) emp
WHERE c <= 3;

-- Ensure we don't push down when the window function's monotonic properties
-- don't match that of the clauses.
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(*) OVER (ORDER BY salary) c
   FROM empsalary) emp
WHERE 3 <= c;

-- Ensure we don't use a run condition when there's a volatile function in the
-- WindowFunc
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(random()) OVER (ORDER BY empno DESC) c
   FROM empsalary) emp
WHERE c = 1;

-- Ensure we don't use a run condition when the WindowFunc contains subplans
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count((SELECT 1)) OVER (ORDER BY empno DESC) c
   FROM empsalary) emp
WHERE c = 1;

--
-- Ensure we get the correct behavior for run condition pushdown when the
-- frame option has an EXCLUDE clause
--

-- Ensure pushdown occurs for ROWS BETWEEN UNBOUNDED PRECEDING with EXCLUDE
-- CURRENT ROW with COUNT(*)
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(*) OVER (ORDER BY salary ROWS BETWEEN UNBOUNDED PRECEDING AND 0 PRECEDING EXCLUDE CURRENT ROW) c
   FROM empsalary) emp
WHERE c <= 3;

-- Ensure pushdown occurs for GROUPS BETWEEN UNBOUNDED PRECEDING with EXCLUDE
-- CURRENT ROW with COUNT(*)
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(*) OVER (ORDER BY salary GROUPS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW EXCLUDE CURRENT ROW) c
   FROM empsalary) emp
WHERE c <= 3;

-- Ensure pushdown occurs for RANGE BETWEEN UNBOUNDED PRECEDING with EXCLUDE
-- CURRENT ROW with COUNT(*)
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(*) OVER (ORDER BY salary RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW EXCLUDE CURRENT ROW) c
   FROM empsalary) emp
WHERE c <= 3;

-- Ensure we don't get pushdown when a FILTER clause is present
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(*) FILTER (WHERE salary > 4000) OVER (ORDER BY salary ROWS BETWEEN UNBOUNDED PRECEDING AND 0 PRECEDING EXCLUDE CURRENT ROW) c
   FROM empsalary) emp
WHERE c <= 3;

-- Ensure we don't get pushdown with COUNT(ANY)
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(salary) OVER (ORDER BY salary ROWS BETWEEN UNBOUNDED PRECEDING AND 0 PRECEDING EXCLUDE CURRENT ROW) c
   FROM empsalary) emp
WHERE c <= 3;

-- Ensure we don't get pushdown with EXCLUDE GROUP
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(*) OVER (ORDER BY salary ROWS BETWEEN UNBOUNDED PRECEDING AND 0 PRECEDING EXCLUDE GROUP) c
   FROM empsalary) emp
WHERE c <= 3;

-- Ensure we don't get pushdown with EXCLUDE TIES
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(*) OVER (ORDER BY salary ROWS BETWEEN UNBOUNDED PRECEDING AND 0 PRECEDING EXCLUDE TIES) c
   FROM empsalary) emp
WHERE c <= 3;

-- Ensure we don't get pushdown with GROUPS mode and EXCLUDE GROUP
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(*) OVER (ORDER BY salary GROUPS BETWEEN UNBOUNDED PRECEDING AND 0 PRECEDING EXCLUDE GROUP) c
   FROM empsalary) emp
WHERE c <= 3;

-- Ensure we don't get pushdown with GROUPS mode and EXCLUDE TIES
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT empno,
          salary,
          count(*) OVER (ORDER BY salary GROUPS BETWEEN UNBOUNDED PRECEDING AND 0 PRECEDING EXCLUDE TIES) c
   FROM empsalary) emp
WHERE c <= 3;


-- Test Sort node collapsing
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT depname,
          sum(salary) OVER (PARTITION BY depname order by empno) depsalary,
          min(salary) OVER (PARTITION BY depname, empno order by enroll_date) depminsalary
   FROM empsalary) emp
WHERE depname = 'sales';

-- Ensure that the evaluation order of the WindowAggs results in the WindowAgg
-- with the same sort order that's required by the ORDER BY is evaluated last.
EXPLAIN (COSTS OFF)
SELECT empno,
       enroll_date,
       depname,
       sum(salary) OVER (PARTITION BY depname order by empno) depsalary,
       min(salary) OVER (PARTITION BY depname order by enroll_date) depminsalary
FROM empsalary
ORDER BY depname, empno;

-- As above, but with an adjusted ORDER BY to ensure the above plan didn't
-- perform only 2 sorts by accident.
EXPLAIN (COSTS OFF)
SELECT empno,
       enroll_date,
       depname,
       sum(salary) OVER (PARTITION BY depname order by empno) depsalary,
       min(salary) OVER (PARTITION BY depname order by enroll_date) depminsalary
FROM empsalary
ORDER BY depname, enroll_date;

SET enable_hashagg TO off;

-- Ensure we don't get a sort for both DISTINCT and ORDER BY.  We expect the
-- sort for the DISTINCT to provide presorted input for the ORDER BY.
EXPLAIN (COSTS OFF)
SELECT DISTINCT
       empno,
       enroll_date,
       depname,
       sum(salary) OVER (PARTITION BY depname order by empno) depsalary,
       min(salary) OVER (PARTITION BY depname order by enroll_date) depminsalary
FROM empsalary
ORDER BY depname, enroll_date;

-- As above but adjust the ORDER BY clause to help ensure the plan with the
-- minimum amount of sorting wasn't a fluke.
EXPLAIN (COSTS OFF)
SELECT DISTINCT
       empno,
       enroll_date,
       depname,
       sum(salary) OVER (PARTITION BY depname order by empno) depsalary,
       min(salary) OVER (PARTITION BY depname order by enroll_date) depminsalary
FROM empsalary
ORDER BY depname, empno;

RESET enable_hashagg;

-- Test Sort node reordering
EXPLAIN (COSTS OFF)
SELECT
  lead(1) OVER (PARTITION BY depname ORDER BY salary, enroll_date),
  lag(1) OVER (PARTITION BY depname ORDER BY salary,enroll_date,empno)
FROM empsalary;

-- Test incremental sorting
EXPLAIN (COSTS OFF)
SELECT * FROM
  (SELECT depname,
          empno,
          salary,
          enroll_date,
          row_number() OVER (PARTITION BY depname ORDER BY enroll_date) AS first_emp,
          row_number() OVER (PARTITION BY depname ORDER BY enroll_date DESC) AS last_emp
   FROM empsalary) emp
WHERE first_emp = 1 OR last_emp = 1;

SELECT * FROM
  (SELECT depname,
          empno,
          salary,
          enroll_date,
          row_number() OVER (PARTITION BY depname ORDER BY enroll_date) AS first_emp,
          row_number() OVER (PARTITION BY depname ORDER BY enroll_date DESC) AS last_emp
   FROM empsalary) emp
WHERE first_emp = 1 OR last_emp = 1;

-- cleanup
DROP TABLE empsalary;

-- test user-defined window function with named args and default args
--dg-skip: relation pg_default_acl cannot be opened: digres has no catalog tables
--dg CREATE FUNCTION nth_value_def(val anyelement, n integer = 1) RETURNS anyelement
--dg   LANGUAGE internal WINDOW IMMUTABLE STRICT AS 'window_nth_value';

--dg-cascade
--dg SELECT nth_value_def(n := 2, val := ten) OVER (PARTITION BY four), ten, four
--dg   FROM (SELECT * FROM tenk1 WHERE unique2 < 10 ORDER BY four, ten) s;

--dg-cascade
--dg SELECT nth_value_def(ten) OVER (PARTITION BY four), ten, four
--dg   FROM (SELECT * FROM tenk1 WHERE unique2 < 10 ORDER BY four, ten) s;

--
-- Test the basic moving-aggregate machinery
--

-- create aggregates that record the series of transform calls (these are
-- intentionally not true inverses)

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION logging_sfunc_nonstrict(text, anyelement) RETURNS text AS
--dg $$ SELECT COALESCE($1, '') || '*' || quote_nullable($2) $$
--dg LANGUAGE SQL IMMUTABLE;

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION logging_msfunc_nonstrict(text, anyelement) RETURNS text AS
--dg $$ SELECT COALESCE($1, '') || '+' || quote_nullable($2) $$
--dg LANGUAGE SQL IMMUTABLE;

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION logging_minvfunc_nonstrict(text, anyelement) RETURNS text AS
--dg $$ SELECT $1 || '-' || quote_nullable($2) $$
--dg LANGUAGE SQL IMMUTABLE;

--dg-cascade
--dg CREATE AGGREGATE logging_agg_nonstrict (anyelement)
--dg (
--dg 	stype = text,
--dg 	sfunc = logging_sfunc_nonstrict,
--dg 	mstype = text,
--dg 	msfunc = logging_msfunc_nonstrict,
--dg 	minvfunc = logging_minvfunc_nonstrict
--dg );

--dg-cascade
--dg CREATE AGGREGATE logging_agg_nonstrict_initcond (anyelement)
--dg (
--dg 	stype = text,
--dg 	sfunc = logging_sfunc_nonstrict,
--dg 	mstype = text,
--dg 	msfunc = logging_msfunc_nonstrict,
--dg 	minvfunc = logging_minvfunc_nonstrict,
--dg 	initcond = 'I',
--dg 	minitcond = 'MI'
--dg );

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION logging_sfunc_strict(text, anyelement) RETURNS text AS
--dg $$ SELECT $1 || '*' || quote_nullable($2) $$
--dg LANGUAGE SQL STRICT IMMUTABLE;

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION logging_msfunc_strict(text, anyelement) RETURNS text AS
--dg $$ SELECT $1 || '+' || quote_nullable($2) $$
--dg LANGUAGE SQL STRICT IMMUTABLE;

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION logging_minvfunc_strict(text, anyelement) RETURNS text AS
--dg $$ SELECT $1 || '-' || quote_nullable($2) $$
--dg LANGUAGE SQL STRICT IMMUTABLE;

--dg-cascade
--dg CREATE AGGREGATE logging_agg_strict (text)
--dg (
--dg 	stype = text,
--dg 	sfunc = logging_sfunc_strict,
--dg 	mstype = text,
--dg 	msfunc = logging_msfunc_strict,
--dg 	minvfunc = logging_minvfunc_strict
--dg );

--dg-cascade
--dg CREATE AGGREGATE logging_agg_strict_initcond (anyelement)
--dg (
--dg 	stype = text,
--dg 	sfunc = logging_sfunc_strict,
--dg 	mstype = text,
--dg 	msfunc = logging_msfunc_strict,
--dg 	minvfunc = logging_minvfunc_strict,
--dg 	initcond = 'I',
--dg 	minitcond = 'MI'
--dg );

-- test strict and non-strict cases
--dg-cascade
--dg SELECT
--dg 	p::text || ',' || i::text || ':' || COALESCE(v::text, 'NULL') AS row,
--dg 	logging_agg_nonstrict(v) over wnd as nstrict,
--dg 	logging_agg_nonstrict_initcond(v) over wnd as nstrict_init,
--dg 	logging_agg_strict(v::text) over wnd as strict,
--dg 	logging_agg_strict_initcond(v) over wnd as strict_init
--dg FROM (VALUES
--dg 	(1, 1, NULL),
--dg 	(1, 2, 'a'),
--dg 	(1, 3, 'b'),
--dg 	(1, 4, NULL),
--dg 	(1, 5, NULL),
--dg 	(1, 6, 'c'),
--dg 	(2, 1, NULL),
--dg 	(2, 2, 'x'),
--dg 	(3, 1, 'z')
--dg ) AS t(p, i, v)
--dg WINDOW wnd AS (PARTITION BY P ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)
--dg ORDER BY p, i;

-- and again, but with filter
--dg-cascade
--dg SELECT
--dg 	p::text || ',' || i::text || ':' ||
--dg 		CASE WHEN f THEN COALESCE(v::text, 'NULL') ELSE '-' END as row,
--dg 	logging_agg_nonstrict(v) filter(where f) over wnd as nstrict_filt,
--dg 	logging_agg_nonstrict_initcond(v) filter(where f) over wnd as nstrict_init_filt,
--dg 	logging_agg_strict(v::text) filter(where f) over wnd as strict_filt,
--dg 	logging_agg_strict_initcond(v) filter(where f) over wnd as strict_init_filt
--dg FROM (VALUES
--dg 	(1, 1, true,  NULL),
--dg 	(1, 2, false, 'a'),
--dg 	(1, 3, true,  'b'),
--dg 	(1, 4, false, NULL),
--dg 	(1, 5, false, NULL),
--dg 	(1, 6, false, 'c'),
--dg 	(2, 1, false, NULL),
--dg 	(2, 2, true,  'x'),
--dg 	(3, 1, true,  'z')
--dg ) AS t(p, i, f, v)
--dg WINDOW wnd AS (PARTITION BY p ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)
--dg ORDER BY p, i;

-- test that volatile arguments disable moving-aggregate mode
--dg-cascade
--dg SELECT
--dg 	i::text || ':' || COALESCE(v::text, 'NULL') as row,
--dg 	logging_agg_strict(v::text)
--dg 		over wnd as inverse,
--dg 	logging_agg_strict(v::text || CASE WHEN random() < 0 then '?' ELSE '' END)
--dg 		over wnd as noinverse
--dg FROM (VALUES
--dg 	(1, 'a'),
--dg 	(2, 'b'),
--dg 	(3, 'c')
--dg ) AS t(i, v)
--dg WINDOW wnd AS (ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)
--dg ORDER BY i;

--dg-cascade
--dg SELECT
--dg 	i::text || ':' || COALESCE(v::text, 'NULL') as row,
--dg 	logging_agg_strict(v::text) filter(where true)
--dg 		over wnd as inverse,
--dg 	logging_agg_strict(v::text) filter(where random() >= 0)
--dg 		over wnd as noinverse
--dg FROM (VALUES
--dg 	(1, 'a'),
--dg 	(2, 'b'),
--dg 	(3, 'c')
--dg ) AS t(i, v)
--dg WINDOW wnd AS (ORDER BY i ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)
--dg ORDER BY i;

-- test that non-overlapping windows don't use inverse transitions
--dg-cascade
--dg SELECT
--dg 	logging_agg_strict(v::text) OVER wnd
--dg FROM (VALUES
--dg 	(1, 'a'),
--dg 	(2, 'b'),
--dg 	(3, 'c')
--dg ) AS t(i, v)
--dg WINDOW wnd AS (ORDER BY i ROWS BETWEEN CURRENT ROW AND CURRENT ROW)
--dg ORDER BY i;

-- test that returning NULL from the inverse transition functions
-- restarts the aggregation from scratch. The second aggregate is supposed
-- to test cases where only some aggregates restart, the third one checks
-- that one aggregate restarting doesn't cause others to restart.

--dg-skip: language "?" does not exist
--dg CREATE FUNCTION sum_int_randrestart_minvfunc(int4, int4) RETURNS int4 AS
--dg $$ SELECT CASE WHEN random() < 0.2 THEN NULL ELSE $1 - $2 END $$
--dg LANGUAGE SQL STRICT;

--dg-cascade
--dg CREATE AGGREGATE sum_int_randomrestart (int4)
--dg (
--dg 	stype = int4,
--dg 	sfunc = int4pl,
--dg 	mstype = int4,
--dg 	msfunc = int4pl,
--dg 	minvfunc = sum_int_randrestart_minvfunc
--dg );

--dg-cascade
--dg WITH
--dg vs AS (
--dg 	SELECT i, (random() * 100)::int4 AS v
--dg 	FROM generate_series(1, 100) AS i
--dg ),
--dg sum_following AS (
--dg 	SELECT i, SUM(v) OVER
--dg 		(ORDER BY i DESC ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS s
--dg 	FROM vs
--dg )
--dg SELECT DISTINCT
--dg 	sum_following.s = sum_int_randomrestart(v) OVER fwd AS eq1,
--dg 	-sum_following.s = sum_int_randomrestart(-v) OVER fwd AS eq2,
--dg 	100*3+(vs.i-1)*3 = length(logging_agg_nonstrict(''::text) OVER fwd) AS eq3
--dg FROM vs
--dg JOIN sum_following ON sum_following.i = vs.i
--dg WINDOW fwd AS (
--dg 	ORDER BY vs.i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
--dg );

--
-- Test various built-in aggregates that have moving-aggregate support
--

-- test inverse transition functions handle NULLs properly
SELECT i,AVG(v::bigint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,AVG(v::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,AVG(v::smallint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,AVG(v::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1.5),(2,2.5),(3,NULL),(4,NULL)) t(i,v);

SELECT i,AVG(v::interval) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,'1 sec'),(2,'2 sec'),(3,NULL),(4,NULL)) t(i,v);

-- moving aggregates over infinite intervals
SELECT  x
        ,avg(x) OVER(ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING ) as curr_next_avg
        ,avg(x) OVER(ROWS BETWEEN 1 PRECEDING AND CURRENT ROW ) as prev_curr_avg
        ,sum(x) OVER(ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING ) as curr_next_sum
        ,sum(x) OVER(ROWS BETWEEN 1 PRECEDING AND CURRENT ROW ) as prev_curr_sum
FROM (VALUES (NULL::interval),
               ('infinity'::interval),
               ('-2147483648 days -2147483648 months -9223372036854775807 usecs'), -- extreme interval value
               ('-infinity'::interval),
               ('2147483647 days 2147483647 months 9223372036854775806 usecs'), -- extreme interval value
               ('infinity'::interval),
               ('6 days'::interval),
               ('7 days'::interval),
               (NULL::interval),
               ('-infinity'::interval)) v(x);

--should fail.
SELECT x, avg(x) OVER(ROWS BETWEEN CURRENT ROW AND 2 FOLLOWING)
FROM (VALUES (NULL::interval),
               ('3 days'::interval),
               ('infinity'::timestamptz - now()),
               ('6 days'::interval),
               ('-infinity'::interval)) v(x);

--should fail.
SELECT x, sum(x) OVER(ROWS BETWEEN CURRENT ROW AND 2 FOLLOWING)
FROM (VALUES (NULL::interval),
               ('3 days'::interval),
               ('infinity'::timestamptz - now()),
               ('6 days'::interval),
               ('-infinity'::interval)) v(x);

SELECT i,SUM(v::smallint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,SUM(v::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,SUM(v::bigint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,SUM(v::money) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,'1.10'),(2,'2.20'),(3,NULL),(4,NULL)) t(i,v);

SELECT i,SUM(v::interval) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,'1 sec'),(2,'2 sec'),(3,NULL),(4,NULL)) t(i,v);

SELECT i,SUM(v::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1.1),(2,2.2),(3,NULL),(4,NULL)) t(i,v);

SELECT SUM(n::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1.01),(2,2),(3,3)) v(i,n);

SELECT i,COUNT(v) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,COUNT(*) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT VAR_POP(n::bigint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VAR_POP(n::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VAR_POP(n::smallint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VAR_POP(n::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VAR_SAMP(n::bigint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VAR_SAMP(n::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VAR_SAMP(n::smallint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VAR_SAMP(n::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VARIANCE(n::bigint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VARIANCE(n::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VARIANCE(n::smallint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT VARIANCE(n::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT STDDEV_POP(n::bigint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,NULL),(2,600),(3,470),(4,170),(5,430),(6,300)) r(i,n);

SELECT STDDEV_POP(n::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,NULL),(2,600),(3,470),(4,170),(5,430),(6,300)) r(i,n);

SELECT STDDEV_POP(n::smallint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,NULL),(2,600),(3,470),(4,170),(5,430),(6,300)) r(i,n);

SELECT STDDEV_POP(n::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,NULL),(2,600),(3,470),(4,170),(5,430),(6,300)) r(i,n);

SELECT STDDEV_SAMP(n::bigint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,NULL),(2,600),(3,470),(4,170),(5,430),(6,300)) r(i,n);

SELECT STDDEV_SAMP(n::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,NULL),(2,600),(3,470),(4,170),(5,430),(6,300)) r(i,n);

SELECT STDDEV_SAMP(n::smallint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,NULL),(2,600),(3,470),(4,170),(5,430),(6,300)) r(i,n);

SELECT STDDEV_SAMP(n::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(1,NULL),(2,600),(3,470),(4,170),(5,430),(6,300)) r(i,n);

SELECT STDDEV(n::bigint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(0,NULL),(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT STDDEV(n::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(0,NULL),(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT STDDEV(n::smallint) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(0,NULL),(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

SELECT STDDEV(n::numeric) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
  FROM (VALUES(0,NULL),(1,600),(2,470),(3,170),(4,430),(5,300)) r(i,n);

-- test that inverse transition functions work with various frame options
SELECT i,SUM(v::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND CURRENT ROW)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,SUM(v::int) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,NULL),(4,NULL)) t(i,v);

SELECT i,SUM(v::int) OVER (ORDER BY i ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING)
  FROM (VALUES(1,1),(2,2),(3,3),(4,4)) t(i,v);

-- ensure aggregate over numeric properly recovers from NaN values
--dg-skip: invalid input syntax for type ?
--dg SELECT a, b,
--dg        SUM(b) OVER(ORDER BY A ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)
--dg FROM (VALUES(1,1::numeric),(2,2),(3,'NaN'),(4,3),(5,4)) t(a,b);

-- It might be tempting for someone to add an inverse trans function for
-- float and double precision. This should not be done as it can give incorrect
-- results. This test should fail if anyone ever does this without thinking too
-- hard about it.
SELECT to_char(SUM(n::float8) OVER (ORDER BY i ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING),'999999999999999999999D9')
  FROM (VALUES(1,1e20),(2,1)) n(i,n);

SELECT i, b, bool_and(b) OVER w, bool_or(b) OVER w
  FROM (VALUES (1,true), (2,true), (3,false), (4,false), (5,true)) v(i,b)
  WINDOW w AS (ORDER BY i ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING);

--
-- Test WindowAgg costing takes into account the number of rows that need to
-- be fetched before the first row can be output.
--

-- Ensure we get a cheap start up plan as the WindowAgg can output the first
-- row after reading 1 row from the join.
--dg-differs: the plan
--dg EXPLAIN (COSTS OFF)
--dg SELECT COUNT(*) OVER (ORDER BY t1.unique1)
--dg FROM tenk1 t1 INNER JOIN tenk1 t2 ON t1.unique1 = t2.tenthous
--dg LIMIT 1;

-- Ensure we get a cheap total plan.  Lack of ORDER BY in the WindowClause
-- means that all rows must be read from the join, so a cheap startup plan
-- isn't a good choice.
EXPLAIN (COSTS OFF)
SELECT COUNT(*) OVER ()
FROM tenk1 t1 INNER JOIN tenk1 t2 ON t1.unique1 = t2.tenthous
WHERE t2.two = 1
LIMIT 1;

-- Ensure we get a cheap total plan.  This time use UNBOUNDED FOLLOWING, which
-- needs to read all join rows to output the first WindowAgg row.
--dg-differs: the plan
--dg EXPLAIN (COSTS OFF)
--dg SELECT COUNT(*) OVER (ORDER BY t1.unique1 ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING)
--dg FROM tenk1 t1 INNER JOIN tenk1 t2 ON t1.unique1 = t2.tenthous
--dg LIMIT 1;

-- Ensure we get a cheap total plan.  This time use 10000 FOLLOWING so we need
-- to read all join rows.
--dg-differs: the plan
--dg EXPLAIN (COSTS OFF)
--dg SELECT COUNT(*) OVER (ORDER BY t1.unique1 ROWS BETWEEN UNBOUNDED PRECEDING AND 10000 FOLLOWING)
--dg FROM tenk1 t1 INNER JOIN tenk1 t2 ON t1.unique1 = t2.tenthous
--dg LIMIT 1;

-- Tests for problems with failure to walk or mutate expressions
-- within window frame clauses.

-- test walker (fails with collation error if expressions are not walked)
SELECT array_agg(i) OVER w
  FROM generate_series(1,5) i
WINDOW w AS (ORDER BY i ROWS BETWEEN (('foo' < 'foobar')::integer) PRECEDING AND CURRENT ROW);

-- test mutator (fails when inlined if expressions are not mutated)
--dg-skip: language "?" does not exist
--dg CREATE FUNCTION pg_temp.f(group_size BIGINT) RETURNS SETOF integer[]
--dg AS $$
--dg     SELECT array_agg(s) OVER w
--dg       FROM generate_series(1,5) s
--dg     WINDOW w AS (ORDER BY s ROWS BETWEEN CURRENT ROW AND GROUP_SIZE FOLLOWING)
--dg $$ LANGUAGE SQL STABLE;

--dg-cascade
--dg EXPLAIN (costs off) SELECT * FROM pg_temp.f(2);
--dg-cascade
--dg SELECT * FROM pg_temp.f(2);
