# Plan 10: age_sql Extension

## Problem / Goal

Two general-purpose AGE utilities needed alongside `age_gds`:

1. **Regexp matching** — AGE's bundled version does not include a `=~`-style
   operator. Need Cypher-callable functions to test and extract regexp matches
   against node/edge property values.

2. **SQL execution from Cypher** — Allow Cypher queries to look up data in
   ordinary PostgreSQL tables (e.g. check if a graphid exists in an external
   table, fetch a property value stored outside the graph). Returns a single row
   as an agtype map so callers can access columns with `.colname`.

Neither feature belongs in `age_gds` (no graph-science content). They are
general AGE utilities, so they live in a new `age_sql` PostgreSQL extension in
its own subdirectory, destined for a separate git repo.

---

## Design

### Extension identity

- **Name**: `age_sql`
- **Location**: `age_sql/` subdirectory of this repo (split to own repo later)
- **Language**: C (PostgreSQL C extension), no C++ needed
- **Dependencies**: PostgreSQL server headers, Apache AGE headers — nothing else

### Cypher callability

AGE passes arguments to SQL functions as `agtype`. All public functions must
accept `agtype` parameters and return `agtype`. Internal helpers may use native
C types.

### Feature 1: Regexp

```sql
-- Returns agtype bool: true if string matches pattern
age_sql.regexp_test(string agtype, pattern agtype, flags agtype DEFAULT NULL)
  RETURNS agtype

-- Returns agtype array of capture groups, or agtype null if no match
age_sql.regexp_match(string agtype, pattern agtype, flags agtype DEFAULT NULL)
  RETURNS agtype
```

Both delegate entirely to PostgreSQL's built-in `regexp_match(text, text, text)`
(which returns `text[]`). The `flags` value passes straight through to
PostgreSQL's POSIX regexp engine — supported flags: `i`, `g`, `s`, `m`, `x`,
`w`, `p`. No custom regexp logic is written.

`regexp_test` calls `regexp_match` and returns true iff the result is non-NULL.

**Example Cypher:**
```cypher
MATCH (n:Person) WHERE age_sql.regexp_test(n.name, 'foo.*', 'i') RETURN n
MATCH (n:Doc) RETURN age_sql.regexp_match(n.body, '(\w+)@(\w+\.com)')
```

### Feature 2: sql_row

```sql
-- Executes query, returns first row as agtype map {colname: value, ...}
-- Returns agtype null if query returns no rows.
-- Allows both read and write queries (same transaction via SPI).
age_sql.sql_row(query agtype, params agtype DEFAULT NULL)
  RETURNS agtype
```

**Named parameter binding** — `$name` placeholders in the query string are
rewritten to positional `$1`, `$2`, … before passing to `SPI_execute_with_args`.
The `params` argument is an agtype map `{name: value, …}`. Order of positional
substitution follows order of first appearance in the query string.

**Type mapping** from agtype param values to SPI `Oid` / `Datum`:

| agtype type | PostgreSQL OID | Datum |
|-------------|---------------|-------|
| string      | `TEXTOID`     | `CStringGetTextDatum` |
| integer     | `INT8OID`     | `Int64GetDatum` |
| float       | `FLOAT8OID`   | `Float8GetDatum` |
| bool        | `BOOLOID`     | `BoolGetDatum` |
| null        | any           | NULL datum, `nulls[i] = 'n'` |

Result columns are converted from PostgreSQL `Datum` back to agtype values and
assembled into an agtype object (map) keyed by column name.

**Example Cypher:**
```cypher
MATCH (n:Person)
WHERE age_sql.sql_row(
    'SELECT count(*) AS cnt FROM orders WHERE customer_id = $gid',
    {gid: id(n)}
).cnt > 0
RETURN n
```

---

## Implementation order

1. Create `age_sql/` directory skeleton: `CMakeLists.txt`, `age_sql.control`,
   `sql/age_sql--1.0.sql`, `src/`, `docs/`, `AGENTS.md`.
2. `src/age_sql.c` — `PG_MODULE_MAGIC`, `_PG_init` stub.
3. SQL DDL: schema, `CREATE FUNCTION` stubs with correct agtype signatures.
4. `src/regexp.c` — `regexp_test` and `regexp_match` implementations.
5. `src/sql_exec.c` — `sql_row` implementation: param parser, SPI call,
   result-to-agtype-map conversion.
6. Build and smoke-test each function from `psql`.

---

## Files to create

All files are new (new extension subdirectory).

| File | Contents |
|------|----------|
| `age_sql/CMakeLists.txt` | PG headers, AGE headers; build `age_sql.so`; install targets |
| `age_sql/age_sql.control` | Extension metadata |
| `age_sql/sql/age_sql--1.0.sql` | Schema + `CREATE FUNCTION` declarations |
| `age_sql/src/age_sql.c` | `PG_MODULE_MAGIC`, `_PG_init` |
| `age_sql/src/regexp.c` | `regexp_test`, `regexp_match` |
| `age_sql/src/sql_exec.c` | `sql_row`, named-param parser, datum→agtype helpers |
| `age_sql/docs/architecture.md` | Extension architecture doc |
| `age_sql/docs/api.md` | SQL API reference |
| `age_sql/AGENTS.md` | Adapted from parent repo (drop NetworKit sections) |

---

## AGENTS.md for age_sql

The `age_sql/AGENTS.md` keeps the plan-first workflow and commit rules.
Drop all NetworKit-specific constraints. Add:

> **Never implement regexp logic from scratch.** All regexp operations must
> delegate to PostgreSQL built-in functions (`regexp_match`, `regexp_test`, etc.)
> via `DirectFunctionCall` or SPI. Do not link any external regexp library.

> **Never construct SQL strings dynamically inside the extension.** The
> `$name → $N` rewriting operates on the *caller-supplied* query template only.
> The extension itself never builds SQL strings by concatenation.

---

## Docs to update

No existing `age_gds` docs need updating (this is a new sibling extension).
New docs to create:

| File | Contents |
|------|----------|
| `age_sql/docs/architecture.md` | Component layout, agtype I/O contract, SPI usage |
| `age_sql/docs/api.md` | All public function signatures with examples |
