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

-- 9: sql_row converts array_agg to a real agtype list
SELECT 1 / (agtype_out(age_sql.sql_row(
    '"SELECT array_agg(x) AS ids FROM (VALUES (1),(2),(3)) v(x)"'::agtype
))::text = '{"ids": [1, 2, 3]}')::int
  AS "Test 9 (sql_row array_agg)";

-- 10: sql_row splices json_agg as a nested array, not a quoted string
SELECT 1 / (agtype_out(age_sql.sql_row(
    '"SELECT json_agg(x) AS ids FROM (VALUES (1),(2),(3)) v(x)"'::agtype
))::text = '{"ids": [1, 2, 3]}')::int
  AS "Test 10 (sql_row json_agg)";

-- 11: sql_row splices a jsonb column unquoted
SELECT 1 / (agtype_out(age_sql.sql_row(
    '"SELECT ''{\"a\":1}''::jsonb AS j"'::agtype
))::text = '{"j": {"a": 1}}')::int
  AS "Test 11 (sql_row jsonb column)";

-- 12: sql_row converts an anonymous record (ROW(...)) to a map
SELECT 1 / (agtype_out(age_sql.sql_row(
    '"SELECT ROW(1, ''a'') AS r"'::agtype
))::text = '{"r": {"f1": 1, "f2": "a"}}')::int
  AS "Test 12 (sql_row anonymous record)";

-- 13: sql_row converts a range column to a {lower,upper,...} object
SELECT 1 / (agtype_out(age_sql.sql_row(
    '"SELECT int4range(1, 5) AS r"'::agtype
))::text = '{"r": {"empty": false, "lower": 1, "upper": 5, "lower_inc": true, "upper_inc": false}}')::int
  AS "Test 13 (sql_row range column)";

-- 14: sql_row handles NULL elements inside an array
SELECT 1 / (agtype_out(age_sql.sql_row(
    '"SELECT array_agg(x) AS ids FROM (VALUES (1),(NULL),(3)) v(x)"'::agtype
))::text = '{"ids": [1, null, 3]}')::int
  AS "Test 14 (sql_row array with NULL element)";

-- 15: sql_row rejects multi-dimensional arrays
DO $$
BEGIN
    PERFORM age_sql.sql_row('"SELECT ''{{1,2},{3,4}}''::int[] AS m"'::agtype);
    RAISE EXCEPTION 'Test 15 (sql_row multi-dim array): expected error was not raised';
EXCEPTION
    WHEN feature_not_supported THEN
        NULL; -- expected
END;
$$;
SELECT 1 AS "Test 15 (sql_row multi-dim array rejected)";

\echo 'All smoke tests passed.'
