#include "postgres.h"
#include "fmgr.h"
#include <ctype.h>
#include "executor/spi.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rangetypes.h"
#include "utils/typcache.h"
#include "utils/agtype.h"

/*
 * Only exported (SQL-callable) AGE symbols are usable from external extensions.
 * Internal helpers like agtype_iterator_init are hidden in age.so.
 */
extern Datum agtype_object_field_text(PG_FUNCTION_ARGS);
extern Datum agtype_out(PG_FUNCTION_ARGS);
extern Datum agtype_in(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(age_sql_sql_row);

/* Parse a JSON/agtype literal string into an agtype Datum. */
static Datum
agtype_from_cstring(const char *str)
{
    return DirectFunctionCall3(agtype_in,
                               CStringGetDatum(str),
                               ObjectIdGetDatum(InvalidOid),
                               Int32GetDatum(-1));
}

/*
 * Extract the string value from an agtype string argument.
 * Uses agtype_out + JSON-quote stripping; cannot use agtype_to_text
 * via LOCAL_FCINFO because agtype_to_text requires a valid flinfo.
 */
static char *
agtype_string_arg(int argnum, PG_FUNCTION_ARGS, const char *func_name)
{
    char *repr = DatumGetCString(DirectFunctionCall1(agtype_out,
                                                     PG_GETARG_DATUM(argnum)));
    size_t len = strlen(repr);

    if (len < 2 || repr[0] != '"' || repr[len - 1] != '"')
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: argument %d must be a string", func_name, argnum + 1)));

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

/*
 * Extract the text value of a named key from an agtype object param map.
 * Returns palloc'd cstring, sets *is_null=true if the field is null or absent.
 * Uses the exported agtype_object_field_text (equiv. to agtype's ->> operator).
 */
static char *
get_param_text(agtype *params_arg, const char *key, bool *is_null)
{
    LOCAL_FCINFO(inner, 2);
    InitFunctionCallInfoData(*inner, NULL, 2, InvalidOid, NULL, NULL);
    inner->args[0].value = PointerGetDatum(params_arg);
    inner->args[0].isnull = false;
    inner->args[1].value = PointerGetDatum(cstring_to_text(key));
    inner->args[1].isnull = false;
    inner->isnull = false;

    Datum result = agtype_object_field_text(inner);
    *is_null = inner->isnull;

    if (inner->isnull)
        return NULL;

    return TextDatumGetCString(result);
}

/*
 * Rewrite $name params to positional $1, $2, ... in order of first appearance.
 * Returns param count; *out_query is the rewritten string; *out_names is the
 * ordered name array.
 * ponytail: simple string scan — a $word inside a literal or $$-quoted block
 * will be incorrectly rewritten. Upgrade to a SQL lexer if that matters.
 */
static int
rewrite_params(const char *query, char **out_query, char ***out_names)
{
    StringInfoData buf;
    List *names = NIL;
    const char *p = query;

    initStringInfo(&buf);

    while (*p)
    {
        if (*p == '$' && (isalpha((unsigned char) p[1]) || p[1] == '_'))
        {
            const char *name_start = p + 1;
            const char *name_end = name_start;

            while (isalnum((unsigned char) *name_end) || *name_end == '_')
                name_end++;

            char *name = pnstrdup(name_start, name_end - name_start);

            int pos = 0;
            ListCell *lc;
            foreach(lc, names)
            {
                pos++;
                if (strcmp((char *) lfirst(lc), name) == 0)
                    break;
            }
            if (lc == NULL)
            {
                names = lappend(names, name);
                pos = list_length(names);
            }

            appendStringInfo(&buf, "$%d", pos);
            p = name_end;
        }
        else
        {
            appendStringInfoChar(&buf, *p++);
        }
    }

    *out_query = buf.data;

    int n = list_length(names);
    if (n > 0)
    {
        *out_names = palloc(n * sizeof(char *));
        int i = 0;
        ListCell *lc;
        foreach(lc, names)
            (*out_names)[i++] = (char *) lfirst(lc);
    }
    else
    {
        *out_names = NULL;
    }

    return n;
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

static void append_json_value(StringInfoData *buf, Datum datum, Oid typid, bool isnull);

/* Append a JSON object {"col1":val1,...} for tuple's columns per tupdesc. */
static void
append_json_object(StringInfoData *buf, TupleDesc tupdesc, HeapTuple tuple)
{
    int ncols = tupdesc->natts;

    appendStringInfoChar(buf, '{');

    for (int i = 0; i < ncols; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        bool isnull;
        Datum datum;

        if (attr->attisdropped)
            continue;

        datum = heap_getattr(tuple, i + 1, tupdesc, &isnull);

        if (i > 0)
            appendStringInfoChar(buf, ',');

        append_json_string(buf, NameStr(attr->attname));
        appendStringInfoChar(buf, ':');
        append_json_value(buf, datum, attr->atttypid, isnull);
    }

    appendStringInfoChar(buf, '}');
}

/*
 * Append a PostgreSQL column value to buf as a JSON value.
 * Numeric and bool types are emitted bare; json/jsonb spliced unquoted;
 * arrays become lists; composites/records become objects; ranges become
 * a {lower,upper,lower_inc,upper_inc,empty} object; everything else is
 * text-quoted.
 */
static void
append_json_value(StringInfoData *buf, Datum datum, Oid typid, bool isnull)
{
    if (isnull)
    {
        appendStringInfoString(buf, "null");
        return;
    }

    switch (typid)
    {
        case BOOLOID:
            appendStringInfoString(buf, DatumGetBool(datum) ? "true" : "false");
            return;
        case INT2OID:
            appendStringInfo(buf, "%d", (int) DatumGetInt16(datum));
            return;
        case INT4OID:
            appendStringInfo(buf, "%d", DatumGetInt32(datum));
            return;
        case INT8OID:
            appendStringInfo(buf, INT64_FORMAT, DatumGetInt64(datum));
            return;
        case FLOAT4OID:
        case FLOAT8OID:
        case NUMERICOID:
        {
            Oid out_func; bool is_varlena;
            getTypeOutputInfo(typid, &out_func, &is_varlena);
            appendStringInfoString(buf, OidOutputFunctionCall(out_func, datum));
            return;
        }
        case JSONOID:
        case JSONBOID:
        {
            Oid out_func; bool is_varlena;
            getTypeOutputInfo(typid, &out_func, &is_varlena);
            appendStringInfoString(buf, OidOutputFunctionCall(out_func, datum));
            return;
        }
        default:
            break;
    }

    Oid elem_typid = get_element_type(typid);

    if (OidIsValid(elem_typid))
    {
        ArrayType *arr = DatumGetArrayTypeP(datum);
        int16 elmlen;
        bool elmbyval;
        char elmalign;
        Datum *elems;
        bool *nulls;
        int nelems;

        if (ARR_NDIM(arr) > 1)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("sql_row: multi-dimensional arrays are not supported")));

        get_typlenbyvalalign(elem_typid, &elmlen, &elmbyval, &elmalign);
        deconstruct_array(arr, elem_typid, elmlen, elmbyval, elmalign,
                           &elems, &nulls, &nelems);

        appendStringInfoChar(buf, '[');
        for (int i = 0; i < nelems; i++)
        {
            if (i > 0)
                appendStringInfoChar(buf, ',');
            append_json_value(buf, elems[i], elem_typid, nulls[i]);
        }
        appendStringInfoChar(buf, ']');
        return;
    }

    char typtype = get_typtype(typid);

    if (typtype == TYPTYPE_COMPOSITE || typid == RECORDOID)
    {
        HeapTupleHeader rec = DatumGetHeapTupleHeader(datum);
        Oid rec_typid = HeapTupleHeaderGetTypeId(rec);
        int32 rec_typmod = HeapTupleHeaderGetTypMod(rec);
        TupleDesc rec_tupdesc = lookup_rowtype_tupdesc(rec_typid, rec_typmod);
        HeapTupleData tmptup;

        tmptup.t_len = HeapTupleHeaderGetDatumLength(rec);
        tmptup.t_data = rec;

        append_json_object(buf, rec_tupdesc, &tmptup);

        ReleaseTupleDesc(rec_tupdesc);
        return;
    }

    if (typtype == TYPTYPE_RANGE)
    {
        TypeCacheEntry *typcache = lookup_type_cache(typid, TYPECACHE_RANGE_INFO);
        RangeType *range = DatumGetRangeTypeP(datum);
        RangeBound lower;
        RangeBound upper;
        bool empty;

        range_deserialize(typcache, range, &lower, &upper, &empty);

        appendStringInfoString(buf, "{\"lower\":");
        if (empty || lower.infinite)
            appendStringInfoString(buf, "null");
        else
            append_json_value(buf, lower.val, typcache->rngelemtype->type_id, false);

        appendStringInfoString(buf, ",\"upper\":");
        if (empty || upper.infinite)
            appendStringInfoString(buf, "null");
        else
            append_json_value(buf, upper.val, typcache->rngelemtype->type_id, false);

        appendStringInfo(buf, ",\"lower_inc\":%s", lower.inclusive ? "true" : "false");
        appendStringInfo(buf, ",\"upper_inc\":%s", upper.inclusive ? "true" : "false");
        appendStringInfo(buf, ",\"empty\":%s}", empty ? "true" : "false");
        return;
    }

    Oid out_func; bool is_varlena;
    getTypeOutputInfo(typid, &out_func, &is_varlena);
    char *str = OidOutputFunctionCall(out_func, datum);
    append_json_string(buf, str);
}

Datum
age_sql_sql_row(PG_FUNCTION_ARGS)
{
    if (PG_ARGISNULL(0))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("sql_row: query must not be null")));

    char *query_str = agtype_string_arg(0, fcinfo, "sql_row");

    agtype *params_arg = NULL;
    if (!PG_ARGISNULL(1))
        params_arg = (agtype *) PG_GETARG_POINTER(1);

    char *rewritten_query;
    char **param_names;
    int nparams = rewrite_params(query_str, &rewritten_query, &param_names);

    Oid *argtypes = NULL;
    Datum *values = NULL;
    char *nulls = NULL;

    if (nparams > 0)
    {
        if (params_arg == NULL)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("sql_row: query has named params but no params map was provided")));

        argtypes = palloc(nparams * sizeof(Oid));
        values = palloc(nparams * sizeof(Datum));
        nulls = palloc(nparams * sizeof(char));

        for (int i = 0; i < nparams; i++)
        {
            bool is_null;
            char *val = get_param_text(params_arg, param_names[i], &is_null);

            if (val == NULL && !is_null)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("sql_row: param \"%s\" not found in params map",
                                param_names[i])));

            argtypes[i] = TEXTOID;
            if (is_null)
            {
                values[i] = (Datum) 0;
                nulls[i] = 'n';
            }
            else
            {
                values[i] = CStringGetTextDatum(val);
                nulls[i] = ' ';
            }
        }
    }

    MemoryContext caller_ctx = CurrentMemoryContext;

    SPI_connect();

    int rc = SPI_execute_with_args(rewritten_query, nparams,
                                   argtypes, values, nulls, false, 1);

    if (rc < 0)
    {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("sql_row: SPI_execute_with_args failed (code %d)", rc)));
    }

    if (SPI_tuptable == NULL || SPI_processed == 0)
    {
        SPI_finish();
        PG_RETURN_DATUM(agtype_from_cstring("null"));
    }

    /* Collect first row into caller context before SPI_finish frees SPI memory */
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    HeapTuple tuple = SPI_tuptable->vals[0];

    MemoryContext old = MemoryContextSwitchTo(caller_ctx);

    /* Build a JSON object string: {"col1": val1, "col2": val2, ...} */
    StringInfoData json;
    initStringInfo(&json);
    append_json_object(&json, tupdesc, tuple);

    MemoryContextSwitchTo(old);

    SPI_finish();

    PG_RETURN_DATUM(agtype_from_cstring(json.data));
}
