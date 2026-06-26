\echo Use "CREATE EXTENSION age_sql" to load this file. \quit

-- Ensure age.so is resident before CREATE FUNCTION validates age_sql.so symbols.
-- PostgreSQL loads a C function's library to validate the symbol at CREATE FUNCTION
-- time; age_sql.so depends on symbols from age.so at dlopen time.
LOAD '$libdir/age';

CREATE FUNCTION age_sql.regexp_test(
    string ag_catalog.agtype,
    pattern ag_catalog.agtype,
    flags ag_catalog.agtype DEFAULT NULL
)
RETURNS ag_catalog.agtype
LANGUAGE c
CALLED ON NULL INPUT
AS '$libdir/age_sql', 'age_sql_regexp_test';

CREATE FUNCTION age_sql.regexp_match(
    string ag_catalog.agtype,
    pattern ag_catalog.agtype,
    flags ag_catalog.agtype DEFAULT NULL
)
RETURNS ag_catalog.agtype
LANGUAGE c
CALLED ON NULL INPUT
AS '$libdir/age_sql', 'age_sql_regexp_match';

CREATE FUNCTION age_sql.sql_row(
    query ag_catalog.agtype,
    params ag_catalog.agtype DEFAULT NULL
)
RETURNS ag_catalog.agtype
LANGUAGE c
CALLED ON NULL INPUT
AS '$libdir/age_sql', 'age_sql_sql_row';
