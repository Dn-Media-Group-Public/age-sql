-- age_sql smoke test — run after CREATE EXTENSION age; CREATE EXTENSION age_sql;
-- Pattern: SELECT 1 / (condition)::int — 1/1=ok, 1/0=fail at runtime.
-- Avoids ELSE 1/0 which PostgreSQL constant-folds at plan time.

LOAD 'age';
SET search_path = ag_catalog, age_sql, "$user", public;

\echo 'Running smoke tests...'

-- 1: regexp_test matches
SELECT 1 / (agtype_out(age_sql.regexp_test('"hello"'::agtype, '"hel.*"'::agtype))::text = 'true')::int
  AS "Test 1 (regexp_test match)";

-- 2: regexp_test case-insensitive
SELECT 1 / (agtype_out(age_sql.regexp_test('"Hello"'::agtype, '"hello"'::agtype, '"i"'::agtype))::text = 'true')::int
  AS "Test 2 (regexp_test i-flag)";

-- 3: regexp_test no match
SELECT 1 / (agtype_out(age_sql.regexp_test('"hello"'::agtype, '"xyz"'::agtype))::text = 'false')::int
  AS "Test 3 (regexp_test no match)";

-- 4: regexp_match returns non-null array on match
SELECT 1 / (agtype_out(age_sql.regexp_match('"user@example.com"'::agtype, '"([^@]+)@([^@]+)"'::agtype))::text <> 'null')::int
  AS "Test 4 (regexp_match match)";

-- 5: regexp_match returns null on no match
SELECT 1 / (agtype_out(age_sql.regexp_match('"hello"'::agtype, '"xyz"'::agtype))::text = 'null')::int
  AS "Test 5 (regexp_match no match)";

-- 6: sql_row returns a row for a simple SELECT
SELECT 1 / (agtype_out(age_sql.sql_row('"SELECT 42 AS answer"'::agtype))::text <> 'null')::int
  AS "Test 6 (sql_row simple)";

-- 7: sql_row with named param
SELECT 1 / (agtype_out(age_sql.sql_row('"SELECT $n::int * 2 AS doubled"'::agtype, '{"n": "5"}'::agtype))::text <> 'null')::int
  AS "Test 7 (sql_row named param)";

-- 8: sql_row returns null when no rows
SELECT 1 / (agtype_out(age_sql.sql_row('"SELECT 1 WHERE false"'::agtype))::text = 'null')::int
  AS "Test 8 (sql_row no rows)";

\echo 'All smoke tests passed.'
