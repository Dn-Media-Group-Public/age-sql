# Building and testing age_sql

## Requirements

| Requirement | Version tested |
|-------------|---------------|
| PostgreSQL | 16 |
| Apache AGE | 1.6.0-rc0 (tag `PG16/v1.6.0-rc0`) |
| CMake | 3.14+ |
| C compiler | GCC or Clang |

AGE headers must be available. If AGE is installed into the PostgreSQL server
include path you do not need to pass `-DAGE_INCLUDEDIR`. If you built AGE from
source, point the build at its `src/include` directory.

---

## Docker (recommended for local testing)

Builds PostgreSQL 16 + AGE + age_sql in an isolated container and runs the
smoke-test suite:

```bash
docker build -t age_sql_test -f docker/Dockerfile .
docker run --rm age_sql_test
```

The build downloads AGE from GitHub (`PG16/v1.6.0-rc0`). Subsequent builds use
Docker layer caching; only the `age_sql/` source layer is rebuilt when you change
extension code.

---

## Manual build (AGE already installed)

```bash
cmake -S age_sql -B build
cmake --build build
sudo cmake --install build
```

If AGE headers are not in the PostgreSQL server include path:

```bash
cmake -S age_sql -B build -DAGE_INCLUDEDIR=/path/to/age/src/include
cmake --build build
sudo cmake --install build
```

Install paths are determined by `pg_config`. The `.so` file goes to
`$(pg_config --pkglibdir)` and the SQL files to
`$(pg_config --sharedir)/extension/`.

---

## Creating the extension in PostgreSQL

```sql
CREATE EXTENSION age;       -- must come first
CREATE EXTENSION age_sql;
```

`age_sql` declares `requires = 'age'` in its control file, so PostgreSQL
enforces the install order.

---

## Running the smoke tests manually

```sql
LOAD 'age';
SET search_path = ag_catalog, age_sql, "$user", public;

\i age_sql/test/smoke.sql
```

The test file uses `ON_ERROR_STOP` semantics via `1 / (condition)::int` —
division by zero means a test failed.
