#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "utils/agtype.h"

/*
 * Only exported (SQL-callable) AGE symbols are available at runtime.
 * Internal helpers like agtype_iterator_init are hidden in age.so.
 * agtype_to_text requires a valid fcinfo->flinfo (used for type OID lookup)
 * and cannot be called via LOCAL_FCINFO with NULL flinfo.
 * We use agtype_out + JSON-quote stripping instead.
 */
extern Datum agtype_out(PG_FUNCTION_ARGS);
extern Datum agtype_in(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(age_sql_regexp_test);
PG_FUNCTION_INFO_V1(age_sql_regexp_match);

/* Parse a JSON/agtype string literal into an agtype Datum. */
static Datum
agtype_from_cstring(const char *str)
{
    return DirectFunctionCall3(agtype_in,
                               CStringGetDatum(str),
                               ObjectIdGetDatum(InvalidOid),
                               Int32GetDatum(-1));
}

/*
 * Extract the text value from an agtype string argument.
 * Returns palloc'd cstring, or ereports if not a string scalar.
 */
/*
 * Extract the text value from an agtype string argument.
 * Uses agtype_out + JSON-quote stripping to avoid the flinfo=NULL
 * limitation of calling agtype_to_text via LOCAL_FCINFO.
 */
static char *
agtype_arg_to_cstring(int argnum, PG_FUNCTION_ARGS, const char *func_name)
{
    char *repr = DatumGetCString(DirectFunctionCall1(agtype_out,
                                                     PG_GETARG_DATUM(argnum)));
    size_t len = strlen(repr);

    if (len < 2 || repr[0] != '"' || repr[len - 1] != '"')
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: argument %d must be a string", func_name, argnum + 1)));

    /* Decode JSON string in-place (strips outer quotes, handles \-escapes) */
    repr[len - 1] = '\0';
    char *result = repr + 1;
    char *dst = result;
    const char *src = result;

    while (*src)
    {
        if (*src == '\\' && *(src + 1))
        {
            src++;
            switch (*src)
            {
                case '"':  *dst++ = '"';  break;
                case '\\': *dst++ = '\\'; break;
                case '/':  *dst++ = '/';  break;
                case 'n':  *dst++ = '\n'; break;
                case 'r':  *dst++ = '\r'; break;
                case 't':  *dst++ = '\t'; break;
                case 'b':  *dst++ = '\b'; break;
                case 'f':  *dst++ = '\f'; break;
                default:   *dst++ = '\\'; *dst++ = *src; break;
            }
            src++;
        }
        else
        {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    return result;
}

/* Escape str as a JSON string value, appending to buf (including the quotes). */
static void
append_json_string(StringInfoData *buf, const char *str)
{
    appendStringInfoChar(buf, '"');
    for (const char *p = str; *p; p++)
    {
        if (*p == '"')        appendStringInfoString(buf, "\\\"");
        else if (*p == '\\')  appendStringInfoString(buf, "\\\\");
        else if (*p == '\n')  appendStringInfoString(buf, "\\n");
        else if (*p == '\r')  appendStringInfoString(buf, "\\r");
        else if (*p == '\t')  appendStringInfoString(buf, "\\t");
        else if ((unsigned char) *p < 0x20)
            appendStringInfo(buf, "\\u%04x", (unsigned char) *p);
        else
            appendStringInfoChar(buf, *p);
    }
    appendStringInfoChar(buf, '"');
}

/* Build an agtype array from a text[] array. */
static Datum
text_array_to_agtype(ArrayType *arr)
{
    Datum *elems;
    bool *nulls;
    int nelems;
    StringInfoData buf;

    deconstruct_array(arr, TEXTOID, -1, false, 'i', &elems, &nulls, &nelems);

    initStringInfo(&buf);
    appendStringInfoChar(&buf, '[');
    for (int i = 0; i < nelems; i++)
    {
        if (i > 0)
            appendStringInfoChar(&buf, ',');
        if (nulls[i])
            appendStringInfoString(&buf, "null");
        else
            append_json_string(&buf, TextDatumGetCString(elems[i]));
    }
    appendStringInfoChar(&buf, ']');

    return agtype_from_cstring(buf.data);
}

/*
 * Call regexp_match via SPI with a fixed query (not dynamic SQL).
 * Returns a copy of the result text[] in caller_ctx, or NULL on no match.
 * ponytail: per-call SPI_connect; cache the plan if throughput matters.
 */
static ArrayType *
spi_regexp_match(const char *str, const char *pat, const char *flags,
                 MemoryContext caller_ctx)
{
    Datum args[3];
    Oid argtypes[3];
    char nulls[3];
    int nargs;
    int rc;

    if (flags != NULL)
    {
        argtypes[0] = TEXTOID; argtypes[1] = TEXTOID; argtypes[2] = TEXTOID;
        args[0] = CStringGetTextDatum(str);
        args[1] = CStringGetTextDatum(pat);
        args[2] = CStringGetTextDatum(flags);
        nulls[0] = nulls[1] = nulls[2] = ' ';
        nargs = 3;
        rc = SPI_execute_with_args("SELECT regexp_match($1, $2, $3)",
                                   nargs, argtypes, args, nulls, true, 1);
    }
    else
    {
        argtypes[0] = TEXTOID; argtypes[1] = TEXTOID;
        args[0] = CStringGetTextDatum(str);
        args[1] = CStringGetTextDatum(pat);
        nulls[0] = nulls[1] = ' ';
        nargs = 2;
        rc = SPI_execute_with_args("SELECT regexp_match($1, $2)",
                                   nargs, argtypes, args, nulls, true, 1);
    }

    if (rc != SPI_OK_SELECT || SPI_processed == 0)
        return NULL;

    bool isnull;
    Datum match_datum = SPI_getbinval(SPI_tuptable->vals[0],
                                      SPI_tuptable->tupdesc, 1, &isnull);
    if (isnull)
        return NULL;

    MemoryContext old = MemoryContextSwitchTo(caller_ctx);
    ArrayType *result = DatumGetArrayTypePCopy(match_datum);
    MemoryContextSwitchTo(old);

    return result;
}

Datum
age_sql_regexp_match(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("regexp_match: string and pattern must not be null")));

    char *str = agtype_arg_to_cstring(0, fcinfo, "regexp_match");
    char *pat = agtype_arg_to_cstring(1, fcinfo, "regexp_match");
    char *flags = NULL;

    if (!PG_ARGISNULL(2))
        flags = agtype_arg_to_cstring(2, fcinfo, "regexp_match");

    MemoryContext caller_ctx = CurrentMemoryContext;

    SPI_connect();
    ArrayType *arr = spi_regexp_match(str, pat, flags, caller_ctx);
    SPI_finish();

    if (arr == NULL)
        PG_RETURN_DATUM(agtype_from_cstring("null"));

    PG_RETURN_DATUM(text_array_to_agtype(arr));
}

Datum
age_sql_regexp_test(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("regexp_test: string and pattern must not be null")));

    char *str = agtype_arg_to_cstring(0, fcinfo, "regexp_test");
    char *pat = agtype_arg_to_cstring(1, fcinfo, "regexp_test");
    char *flags = NULL;

    if (!PG_ARGISNULL(2))
        flags = agtype_arg_to_cstring(2, fcinfo, "regexp_test");

    MemoryContext caller_ctx = CurrentMemoryContext;

    SPI_connect();
    ArrayType *arr = spi_regexp_match(str, pat, flags, caller_ctx);
    SPI_finish();

    PG_RETURN_DATUM(agtype_from_cstring(arr != NULL ? "true" : "false"));
}
