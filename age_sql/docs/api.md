# age_sql — SQL / Cypher API Reference

See also: [Plan: age_sql Extension](../../docs/implemented-plans/age-sql-extension.md)

All functions live in the `age_sql` schema and accept `ag_catalog.agtype`
arguments so they can be called directly from Cypher expressions.

---

## age_sql.regexp_test

```sql
age_sql.regexp_test(
    string  agtype,
    pattern agtype,
    flags   agtype DEFAULT NULL
) RETURNS agtype   -- boolean true / false
```

Returns agtype `true` if `string` matches the POSIX regexp `pattern`, agtype
`false` otherwise. Delegates to PostgreSQL's built-in `regexp_match`.

`flags` is an optional string of PostgreSQL regexp flag characters:

| Flag | Meaning |
|------|---------|
| `i`  | Case-insensitive |
| `s`  | Dot matches newline |
| `m`  | `^`/`$` match line boundaries |
| `x`  | Allow whitespace/comments in pattern |
| `g`  | Global (find all matches — only relevant for `regexp_match`) |

**Cypher example:**

```cypher
MATCH (n:Person)
WHERE age_sql.regexp_test(n.name, '^alice', 'i')
RETURN n.name
```

**SQL example:**

```sql
SELECT age_sql.regexp_test('"hello world"', '"hel.*"');
-- → true
```

---

## age_sql.regexp_match

```sql
age_sql.regexp_match(
    string  agtype,
    pattern agtype,
    flags   agtype DEFAULT NULL
) RETURNS agtype   -- array of capture-group strings, or null if no match
```

Returns an agtype array of capture group strings if `string` matches `pattern`,
or agtype `null` if there is no match. Delegates to PostgreSQL's built-in
`regexp_match`.

**Cypher example:**

```cypher
MATCH (n:Email)
RETURN age_sql.regexp_match(n.address, '^([^@]+)@([^@]+)$')
-- → ["user", "example.com"]  or null
```

**SQL example:**

```sql
SELECT age_sql.regexp_match('"user@example.com"', '"([^@]+)@([^@]+)"');
-- → ["user", "example.com"]
```

> **Pattern syntax note:** agtype string literals use JSON encoding. Backslash
> sequences inside the pattern must be valid JSON escapes. To match a literal
> digit class use `[0-9]` rather than `\d`; to match word characters use
> `[A-Za-z0-9_]` rather than `\w`.

---

## age_sql.sql_row

```sql
age_sql.sql_row(
    query  agtype,
    params agtype DEFAULT NULL
) RETURNS agtype   -- object {colname: value, …}, or null if no rows
```

See also: [Plan: sql_row Nested Value Conversion](../../docs/implemented-plans/sql-row-nested-value-conversion.md)

Executes `query` in the current transaction via SPI and returns the first row
as an agtype object keyed by column name. Returns agtype `null` if the query
returns no rows.

**Named parameters** — `$name` placeholders in the query template are rewritten
to positional `$1`, `$2`, … in order of first appearance. The `params` argument
must be an agtype object `{name: "value", …}`. All parameter values are passed
as text and the query must cast them as needed (e.g. `$id::int`).

**Return value** — column values are mapped to agtype as follows, recursively:

| PostgreSQL type | agtype value |
|-----------------|-------------|
| `bool` | `true` / `false` |
| `int2`, `int4`, `int8` | integer |
| `float4`, `float8`, `numeric` | float |
| `json`, `jsonb` | spliced in as a real nested agtype value (not a quoted string) |
| 1-D array (`int[]`, `array_agg(...)`, ...) | agtype list, elements converted recursively; `NULL` elements become `null` |
| composite / row type, incl. anonymous `ROW(...)` records | agtype map `{field: value, ...}`, fields converted recursively |
| range type (`int4range`, `tstzrange`, ...) | agtype map `{"lower":..., "upper":..., "lower_inc":bool, "upper_inc":bool, "empty":bool}` — agtype has no native range type, so bounds are `null` when infinite or the range is empty |
| everything else | string (via type output function) |
| NULL | `null` |

> **Limitation:** multi-dimensional arrays (`ARR_NDIM > 1`) are not supported
> and raise an error — only 1-D arrays convert to agtype lists.

**Cypher example:**

```cypher
MATCH (p:Person)
WHERE age_sql.sql_row(
    'SELECT 1 FROM orders WHERE customer_id = $id LIMIT 1',
    {id: toString(id(p))}
) IS NOT NULL
RETURN p.name
```

**SQL example:**

```sql
SELECT age_sql.sql_row('"SELECT 42 AS answer"');
-- → {"answer": 42}

SELECT age_sql.sql_row(
    '"SELECT $n::int * 2 AS doubled"',
    '{"n": "5"}'
);
-- → {"doubled": 10}
```

> **Limitation:** the `$name` rewriter is a simple string scan. A `$word` that
> appears inside a SQL string literal or dollar-quoted block in the query
> template will be incorrectly rewritten. Avoid such patterns.
