# age_sql

A thin PostgreSQL C extension providing general-purpose Apache AGE utilities.

## Features

- **Regexp matching** — Cypher-callable `regexp_test` and `regexp_match` functions, delegating to PostgreSQL's built-in POSIX regexp engine.
- **SQL execution from Cypher** — `sql_row` executes a parameterised SQL query from within a Cypher expression and returns the first row as an agtype map.

## Quick example

```sql
-- Setup
CREATE EXTENSION age;
CREATE EXTENSION age_sql;
LOAD 'age';
SET search_path = ag_catalog, age_sql, "$user", public;

SELECT * FROM cypher('my_graph', $$

  -- Find people whose names start with "Alice" (case-insensitive)
  MATCH (p:Person)
  WHERE age_sql.regexp_test(p.name, '^alice', 'i')
  RETURN p.name

$$) AS (name agtype);

SELECT * FROM cypher('my_graph', $$

  -- Join graph node to a SQL table: return nodes that have orders
  MATCH (p:Person)
  WHERE age_sql.sql_row(
    'SELECT 1 FROM orders WHERE customer_id = $id LIMIT 1',
    {id: toString(id(p))}
  ) IS NOT NULL
  RETURN p.name

$$) AS (name agtype);
```

## Documentation

- [Build & test instructions](docs/build.md)
- [SQL / Cypher API reference](age_sql/docs/api.md)
- [Architecture](age_sql/docs/architecture.md)
