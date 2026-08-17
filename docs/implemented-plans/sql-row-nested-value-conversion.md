# `sql_row` Nested Value Conversion (arrays, json/jsonb, composites, ranges)

## Problem / Goal

`age_sql.sql_row`'s column-to-agtype conversion (`append_json_value` in `age_sql/src/sql_exec.c`) only special-cases scalar Oids: `BOOLOID`, `INT2/4/8OID`, `FLOAT4/8OID`, `NUMERICOID`. Every other column type — including `int[]`/`bigint[]`, `array_agg(...)`, `json_agg(...)`, `json`/`jsonb`, composite/row types, and range types — falls into the `default:` branch, which calls the type's normal text output function and then **wraps that text as a quoted JSON string**.

This silently destroys structure instead of erroring:
- `array_agg(gid) AS ids` → Postgres array literal text `{1,2,3}` → agtype string `"{1,2,3}"` (not a list)
- `json_agg(gid) AS ids` → JSON text `[1,2,3]` → agtype string `"[1,2,3]"` (double-encoded, not a nested array)
- A composite/row-typed column → its text representation `(1,foo)` → agtype string `"(1,foo)"`, fields inaccessible

Since Cypher has no JSON-parsing function, a caller cannot turn `"{1,2,3}"` back into a real list to use as `id(n) IN [...]`. This blocks a useful pattern: returning a *set* of ids from one `sql_row` call for Cypher to consume directly, e.g.:

```cypher
WITH age_sql.sql_row('SELECT array_agg(gid) AS ids FROM entity_time_range WHERE ...') AS f
MATCH (n) WHERE id(n) IN f.ids
RETURN n
```

Goal: make `sql_row` produce real nested agtype values (lists, maps) for array, json/jsonb, composite, and range columns, recursively, instead of stringifying them.

## Design

### Scope: general recursive converter

Rather than special-casing just arrays and json (the two cases that motivated this), generalize `append_json_value` into a proper recursive Postgres-datum → JSON-text converter that handles:

1. **json/jsonb** — the column's text output is already valid JSON. Splice it into the buffer **unquoted** (instead of the current `append_json_string` quoting), so `agtype_from_cstring` parses it as a genuine nested agtype value.
2. **Arrays (1-D only)** — detect via `get_element_type(typid) != InvalidOid`. Use `deconstruct_array` (with `get_typlenbyvalalign` on the element type) to get the element `Datum[]`/`bool[] nulls`, recurse `append_json_value` per element, join as `[e1,e2,...]`. NULL elements become JSON `null` via the existing `isnull` path.
   - **Multi-dimensional arrays (`ARR_NDIM > 1`) are rejected** with a clear `ereport(ERROR, ...)` rather than silently mis-nesting or flattening. `ponytail: 1-D arrays only — the graph/query use cases here are id lists, not matrices; add N-D nesting via ARR_DIMS-driven recursion if a real need shows up.`
3. **Composite / row types** — detect via `get_typtype(typid) == TYPTYPE_COMPOSITE`. Derive the *actual* tupdesc from the datum itself (`DatumGetHeapTupleHeader` → `HeapTupleHeaderGetTypeId`/`GetTypMod` → `lookup_rowtype_tupdesc`), not from the passed-in static `typid`/`typmod` — this makes it work for both named composite types and anonymous `RECORD`s (e.g. `ROW(1, 'a')`). Recurse per field, join as `{"field1":v1,...}`, matching the exact same shape as the existing top-level row conversion.
   - Factor the existing top-level "walk a `TupleDesc` + `HeapTuple`, emit a JSON object" loop (`age_sql_sql_row`, current lines 350–362) into a shared static helper `append_json_object(StringInfoData *buf, TupleDesc tupdesc, HeapTuple tuple)`, reused by both the top-level row and this composite-field case. No behavior change for the top-level case.
4. **Range types** — detect via `get_typtype(typid) == TYPTYPE_RANGE`. Use `lookup_type_cache(typid, TYPECACHE_RANGE_INFO)` + `range_deserialize` to get lower/upper bounds and inclusivity/empty flags. Emit as a plain JSON object: `{"lower":..., "upper":..., "lower_inc":bool, "upper_inc":bool, "empty":bool}`, recursing on the bound values via the range's element type (`rngtypcache->rngelemtype`). agtype has no native range type, so this is the closest lossless representation, not a 1:1 Postgres-range analog.
5. **Everything else** (text, uuid, bytea, date/time/timestamp, enum, etc.) — unchanged: existing text-output + `append_json_string` quoting fallback.

### Compatibility

No known caller depends on the current (broken) stringified output for arrays/json/composite/range columns — treat this as a straight bugfix, no compatibility shim, no dual-mode flag. Scalar-column behavior (the existing whitelist) is unchanged.

No change to `sql_row`'s SQL-level signature or the `$name` param rewriting — this is entirely internal to `append_json_value`/the new helper.

## Implementation order

1. Add type-classification checks using existing `lsyscache.h`/`utils/array.h`/`utils/rangetypes.h`/`utils/typcache.h` helpers: `get_element_type`, `get_typtype`, `TYPTYPE_COMPOSITE`, `TYPTYPE_RANGE`.
2. Factor `age_sql_sql_row`'s row-building loop (lines 350–362) into `static void append_json_object(StringInfoData *buf, TupleDesc tupdesc, HeapTuple tuple)`; call it from `age_sql_sql_row` unchanged.
3. Extend `append_json_value`:
   - json/jsonb branch: unquoted splice.
   - array branch: `deconstruct_array` + per-element recursion + `ARR_NDIM > 1` → `ereport(ERROR, ...)`.
   - composite branch: derive tupdesc from the datum's `HeapTupleHeader`, wrap into a `HeapTupleData`, call `append_json_object`, `ReleaseTupleDesc`.
   - range branch: `range_deserialize` + recurse on bounds, emit `{lower,upper,lower_inc,upper_inc,empty}`.
4. Add `#include "utils/array.h"`, `"utils/rangetypes.h"`, `"utils/typcache.h"`, `"access/htup_details.h"` (already included) as needed.
5. Extend `age_sql/test/smoke.sql` with cases: `array_agg(int)`, `json_agg(...)`, a `jsonb` column, `ROW(1,'a')` (anonymous record), a named composite column if one exists in test fixtures, a range column (`int4range`/`tstzrange`), an array containing NULLs, and a multi-dimensional array asserting the new error.
6. Update `age_sql/docs/api.md`'s `sql_row` section to document the value-conversion rules (scalar passthrough, json/jsonb spliced, arrays → lists with the 1-D-only caveat, composites → maps, ranges → `{lower,upper,lower_inc,upper_inc,empty}`).

## Files to change

| File | Change |
|---|---|
| `age_sql/src/sql_exec.c` | Factor `append_json_object` helper; extend `append_json_value` with json/jsonb, array, composite, and range branches |
| `age_sql/test/smoke.sql` | Add test cases for each new branch, NULL array elements, and the multi-dim-array error |
| `age_sql/docs/api.md` | Document `sql_row`'s per-type conversion rules |

## Docs to update

- `age_sql/docs/api.md` — `sql_row` value-conversion section (see above)
- `docs/implemented-plans/index.md` — add entry when plan is moved
