# age_sql — Agent rules

## Development workflow

Follow the plan-first workflow from the parent repo's CLAUDE.md:
- Write a plan in `docs/plans/<feature>.md` before touching code.
- Implement only after the user approves the plan.
- Move completed plans to `docs/implemented-plans/` and update the index.
- Never commit to git. Only the user does.

## Non-negotiable constraints

**Never implement regexp logic from scratch.** All regexp operations must delegate
to PostgreSQL built-in functions (`regexp_match`, `regexp_like`, etc.) via SPI or
`DirectFunctionCall`. Do not link any external regexp library.

**Never construct SQL strings dynamically inside the extension.** The
`$name → $N` rewriting in `sql_exec.c` operates on the *caller-supplied* query
template only. The extension itself never builds SQL strings by concatenation.

## Build

```
cmake -S age_sql -B build && cmake --build build && cmake --install build
psql -c "CREATE EXTENSION age_sql"
```

Requires `pg_config` in PATH and Apache AGE installed (headers under
`$(pg_config --includedir-server)`).
