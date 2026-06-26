# age_sql — Architecture

See also: [Plan: age_sql Extension](../../docs/implemented-plans/age-sql-extension.md)

## Components

```
age_sql/
├── src/age_sql.c    — PG_MODULE_MAGIC, _PG_init stub
├── src/regexp.c     — regexp_test, regexp_match (delegates to PG via SPI)
└── src/sql_exec.c   — sql_row ($name rewriting, SPI execution, result mapping)
```

## AGE symbol visibility constraint

Only SQL-callable (exported) AGE symbols are available to an external extension
at runtime. AGE's internal helpers (`agtype_iterator_init`, `push_agtype_value`,
`agtype_value_to_agtype`, `AG_GET_ARG_AGTYPE_P`, etc.) are compiled with hidden
visibility and cannot be resolved by `dlopen`.

This extension uses only two AGE symbols:

| Symbol | Signature | Used for |
|--------|-----------|----------|
| `agtype_in` | `(cstring, oid, int4) → agtype` | Build an agtype return value from a JSON literal |
| `agtype_out` | `(agtype) → cstring` | Serialise an agtype argument to its JSON text representation |

Both are the standard type I/O functions for the `agtype` type and are always
exported.

## agtype I/O contract

**Inputs** — public functions receive `agtype` arguments. To extract a string
value, `agtype_out` is called via `DirectFunctionCall1`, producing the agtype
JSON text representation (e.g. `"hello"` for a string scalar). The outer JSON
double-quotes are stripped and JSON escape sequences are decoded in-place to
yield the plain text value.

`agtype_out` is chosen over `agtype_to_text` because `agtype_to_text` accesses
`fcinfo->flinfo` to resolve argument type OIDs. When called via `LOCAL_FCINFO`
with `flinfo = NULL`, it throws "could not determine data type for argument 1".
`agtype_out` has no such dependency.

**Outputs** — return values are built by calling `agtype_in` via
`DirectFunctionCall3` with a JSON literal string (e.g. `"true"`, `"false"`,
`"null"`, or a hand-built `{...}` / `[...]` JSON string).

## SPI memory safety

SPI creates its own memory context. Any pointer into `SPI_tuptable` is freed at
`SPI_finish()`. Pattern used in both `regexp.c` and `sql_exec.c`:

1. Save `caller_ctx = CurrentMemoryContext` before `SPI_connect()`.
2. Execute the query.
3. Switch to `caller_ctx` with `MemoryContextSwitchTo(caller_ctx)` and copy any
   result data (arrays, column values, text) that must survive `SPI_finish()`.
4. Switch back, then call `SPI_finish()`.
5. Build the agtype return value (data already in `caller_ctx`).

## Named parameter rewriting

`sql_exec.c::rewrite_params` scans the caller-supplied query template for
`$identifier` tokens, rewrites them to `$1`, `$2`, … (first-appearance order),
and returns the ordered name list. The rewritten query is passed verbatim to
`SPI_execute_with_args`; no SQL is ever constructed by concatenation from
user-controlled data.

Limitation: a `$word` that appears inside a SQL string literal or
dollar-quoted block will be incorrectly rewritten. Upgrade to a full SQL lexer
if that matters.
