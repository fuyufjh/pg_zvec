#include "pg_zvec.h"
#include "pg_zvec_shmem.h"
#include "zvec_bridge/zvec_bridge.h"

#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "commands/trigger.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/wait_event.h"

/* ----------------------------------------------------------------
 * send_worker_request
 *
 * Posts a request to the background worker and blocks until a
 * response arrives (or `timeout_ms` milliseconds elapse).
 *
 * Phase 1 note: single-slot serialised IPC; only one backend may
 * have a request in flight at a time.  Concurrent backends will
 * get "worker is busy; try again later".
 * ---------------------------------------------------------------- */
static bool
send_worker_request(ZvecRequest *req, ZvecResponse *resp,
                    int timeout_ms, char *errbuf, int errbuf_len)
{
    int waited = 0;

    if (!pg_zvec_state)
    {
        snprintf(errbuf, errbuf_len,
                 "pg_zvec shared memory not initialised");
        return false;
    }

    /* Acquire the single request slot */
    LWLockAcquire(pg_zvec_state->lock, LW_EXCLUSIVE);

    if (!pg_zvec_state->worker_ready)
    {
        LWLockRelease(pg_zvec_state->lock);
        snprintf(errbuf, errbuf_len, "pg_zvec worker is not running");
        return false;
    }

    if (pg_zvec_state->request_pending)
    {
        LWLockRelease(pg_zvec_state->lock);
        snprintf(errbuf, errbuf_len,
                 "pg_zvec worker is busy; try again later");
        return false;
    }

    req->sender_pid = MyProcPid;
    memcpy(&pg_zvec_state->request, req, sizeof(ZvecRequest));
    pg_zvec_state->request_pending = true;
    pg_zvec_state->response_ready  = false;

    /* Wake the worker */
    if (pg_zvec_state->worker_pid > 0)
    {
        PGPROC *worker = BackendPidGetProc(pg_zvec_state->worker_pid);
        if (worker)
            SetLatch(&worker->procLatch);
    }

    LWLockRelease(pg_zvec_state->lock);

    /* Poll until response is ready or timeout */
    while (waited < timeout_ms)
    {
        int rc;

        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       100L,
                       PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);

        if (rc & WL_EXIT_ON_PM_DEATH)
        {
            snprintf(errbuf, errbuf_len, "postmaster died");
            return false;
        }

        LWLockAcquire(pg_zvec_state->lock, LW_SHARED);
        if (pg_zvec_state->response_ready)
        {
            memcpy(resp, &pg_zvec_state->response, sizeof(ZvecResponse));
            LWLockRelease(pg_zvec_state->lock);
            if (!resp->success)
                snprintf(errbuf, errbuf_len, "%s", resp->error_msg);
            return resp->success;
        }
        LWLockRelease(pg_zvec_state->lock);

        waited += 100;
    }

    snprintf(errbuf, errbuf_len, "timed out waiting for pg_zvec worker");
    return false;
}

/*
 * compute_data_dir: if the caller supplied a non-empty data_dir, use it.
 * Otherwise compose $PGDATA/pg_zvec/<collection_name>.
 */
static void
compute_data_dir(const char *collection_name, const char *arg_data_dir,
                 char *out, int outsz)
{
    if (arg_data_dir && arg_data_dir[0] != '\0')
        strlcpy(out, arg_data_dir, outsz);
    else if (pg_zvec_data_dir && pg_zvec_data_dir[0] != '\0')
        snprintf(out, outsz, "%s/%s", pg_zvec_data_dir, collection_name);
    else
        snprintf(out, outsz, "%s/pg_zvec/%s", DataDir, collection_name);
}

/* ----------------------------------------------------------------
 * zvec_create_collection(collection_name, table_name, vector_column,
 *                         dimension, metric, index_type, params, data_dir)
 *
 * $1  collection_name text
 * $2  table_name      text
 * $3  vector_column   text
 * $4  dimension       integer
 * $5  metric          text    DEFAULT 'cosine'
 * $6  index_type      text    DEFAULT 'hnsw'
 * $7  params          json    DEFAULT '{}'
 * $8  data_dir        text    DEFAULT ''
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(zvec_create_collection);
Datum
zvec_create_collection(PG_FUNCTION_ARGS)
{
    char        *col_name   = text_to_cstring(PG_GETARG_TEXT_PP(0));
    /* arg1 (table_name) stored in shared entry for future use */
    int          dimension  = PG_GETARG_INT32(3);
    char        *metric     = text_to_cstring(PG_GETARG_TEXT_PP(4));
    char        *index_type = text_to_cstring(PG_GETARG_TEXT_PP(5));
    char        *params_str = text_to_cstring(PG_GETARG_TEXT_PP(6));
    char        *arg_dir    = text_to_cstring(PG_GETARG_TEXT_PP(7));

    char         data_dir[ZVEC_MAX_PATH_LEN];
    char         errbuf[256];
    ZvecRequest  req;
    ZvecResponse resp;
    int          pos = 0;

    if (dimension <= 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("dimension must be a positive integer")));

    compute_data_dir(col_name, arg_dir, data_dir, sizeof(data_dir));

    /* Pack payload */
    memset(&req, 0, sizeof(req));
    req.type        = ZVEC_REQ_CREATE_COLLECTION;
    req.database_id = MyDatabaseId;

    pos = zvec_pack_str(req.data, pos, sizeof(req.data), col_name);
    if (pos < 0) goto overflow;
    pos = zvec_pack_str(req.data, pos, sizeof(req.data), data_dir);
    if (pos < 0) goto overflow;
    pos = zvec_pack_str(req.data, pos, sizeof(req.data), index_type);
    if (pos < 0) goto overflow;
    pos = zvec_pack_str(req.data, pos, sizeof(req.data), metric);
    if (pos < 0) goto overflow;
    pos = zvec_pack_int(req.data, pos, sizeof(req.data), dimension);
    if (pos < 0) goto overflow;
    pos = zvec_pack_str(req.data, pos, sizeof(req.data), params_str);
    if (pos < 0) goto overflow;
    req.data_len = pos;

    if (!send_worker_request(&req, &resp, 10000, errbuf, sizeof(errbuf)))
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("zvec_create_collection failed: %s", errbuf)));

    PG_RETURN_VOID();

overflow:
    ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("zvec_create_collection: request buffer overflow")));
}

/* ----------------------------------------------------------------
 * zvec_drop_collection(collection_name text)
 * Payload: [name\0]
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(zvec_drop_collection);
Datum
zvec_drop_collection(PG_FUNCTION_ARGS)
{
    char        *col_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char         errbuf[256];
    ZvecRequest  req;
    ZvecResponse resp;
    int          pos = 0;

    memset(&req, 0, sizeof(req));
    req.type        = ZVEC_REQ_DROP_COLLECTION;
    req.database_id = MyDatabaseId;

    pos = zvec_pack_str(req.data, pos, sizeof(req.data), col_name);
    if (pos < 0)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("zvec_drop_collection: buffer overflow")));
    req.data_len = pos;

    if (!send_worker_request(&req, &resp, 10000, errbuf, sizeof(errbuf)))
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("zvec_drop_collection failed: %s", errbuf)));

    PG_RETURN_VOID();
}

/* ----------------------------------------------------------------
 * zvec_optimize(collection_name text)
 * Payload: [name\0]
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(zvec_optimize);
Datum
zvec_optimize(PG_FUNCTION_ARGS)
{
    char        *col_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char         errbuf[256];
    ZvecRequest  req;
    ZvecResponse resp;
    int          pos = 0;

    memset(&req, 0, sizeof(req));
    req.type        = ZVEC_REQ_OPTIMIZE;
    req.database_id = MyDatabaseId;

    pos = zvec_pack_str(req.data, pos, sizeof(req.data), col_name);
    if (pos < 0)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("zvec_optimize: buffer overflow")));
    req.data_len = pos;

    if (!send_worker_request(&req, &resp, 300000, errbuf, sizeof(errbuf)))
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("zvec_optimize failed: %s", errbuf)));

    PG_RETURN_VOID();
}

/* ----------------------------------------------------------------
 * zvec_search / zvec_search_filtered
 *   → TABLE(pk text, score float4)
 *
 * Search is performed directly in the backend by opening a
 * read-only handle to the collection's data directory.
 * This avoids routing large query vectors through the IPC slot.
 * ---------------------------------------------------------------- */

typedef struct ZvecSearchCtx
{
    ZvecSearchResult *results;
    int               num_results;
} ZvecSearchCtx;

static Datum
zvec_search_internal(PG_FUNCTION_ARGS,
                     const char *col_name,
                     float4     *query_data,
                     int         query_len,
                     int         topk,
                     const char *filter_expr)
{
    FuncCallContext *funcctx;
    ZvecSearchCtx   *ctx;

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext   oldctx;
        TupleDesc       tupdesc;
        ZvecCollectionEntry *entry = NULL;
        char             data_dir[ZVEC_MAX_PATH_LEN];
        char             errbuf[256];
        ZvecCollectionHandle *h;
        ZvecSearchResult *results;
        int              nresults;

        funcctx = SRF_FIRSTCALL_INIT();
        oldctx  = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* Build tuple descriptor: (pk text, score float4) */
        tupdesc = CreateTemplateTupleDesc(2);
        TupleDescInitEntry(tupdesc, (AttrNumber) 1, "pk",    TEXTOID,   -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 2, "score", FLOAT4OID, -1, 0);
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);

        /* Look up data_dir from shared-memory registry */
        if (!pg_zvec_state)
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("pg_zvec shared memory not available")));

        LWLockAcquire(pg_zvec_state->lock, LW_SHARED);
        for (int i = 0; i < ZVEC_MAX_COLLECTIONS; i++)
        {
            if (pg_zvec_state->collections[i].in_use &&
                pg_zvec_state->collections[i].database_id == MyDatabaseId &&
                strcmp(pg_zvec_state->collections[i].name, col_name) == 0)
            {
                strlcpy(data_dir,
                        pg_zvec_state->collections[i].data_dir,
                        sizeof(data_dir));
                entry = &pg_zvec_state->collections[i];
                break;
            }
        }
        LWLockRelease(pg_zvec_state->lock);

        if (!entry)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("zvec collection \"%s\" not found", col_name)));

        if (topk <= 0)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("topk must be a positive integer")));

        /* Open read-only handle directly in this backend process */
        h = zvec_collection_open(data_dir, true, errbuf, sizeof(errbuf));
        if (!h)
            ereport(ERROR,
                    (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                     errmsg("zvec_search: failed to open collection: %s",
                            errbuf)));

        results = (ZvecSearchResult *) palloc(topk * sizeof(ZvecSearchResult));
        nresults = zvec_collection_search(h,
                                          query_data, query_len,
                                          topk,
                                          filter_expr,
                                          results,
                                          errbuf, sizeof(errbuf));
        zvec_collection_close(h);

        if (nresults < 0)
            ereport(ERROR,
                    (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                     errmsg("zvec_search failed: %s", errbuf)));

        ctx = (ZvecSearchCtx *) palloc(sizeof(ZvecSearchCtx));
        ctx->results     = results;
        ctx->num_results = nresults;

        funcctx->user_fctx = ctx;
        funcctx->max_calls = (uint64) nresults;

        MemoryContextSwitchTo(oldctx);
    }

    funcctx = SRF_PERCALL_SETUP();
    ctx = (ZvecSearchCtx *) funcctx->user_fctx;

    if (funcctx->call_cntr < funcctx->max_calls)
    {
        int        i      = (int) funcctx->call_cntr;
        Datum      values[2];
        bool       nulls[2] = {false, false};
        HeapTuple  tuple;

        values[0] = CStringGetTextDatum(ctx->results[i].pk);
        values[1] = Float4GetDatum(ctx->results[i].score);
        tuple     = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }

    SRF_RETURN_DONE(funcctx);
}

PG_FUNCTION_INFO_V1(zvec_search);
Datum
zvec_search(PG_FUNCTION_ARGS)
{
    char       *col_name  = text_to_cstring(PG_GETARG_TEXT_PP(0));
    ArrayType  *query_arr = PG_GETARG_ARRAYTYPE_P(1);
    int         topk      = PG_GETARG_INT32(2);
    float4     *qdata;
    int         qlen;

    if (ARR_NDIM(query_arr) != 1)
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("query must be a 1-D float4 array")));
    if (ARR_ELEMTYPE(query_arr) != FLOAT4OID)
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("query must be float4[]")));
    if (ARR_HASNULL(query_arr))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("query vector must not contain NULLs")));

    qdata = (float4 *) ARR_DATA_PTR(query_arr);
    qlen  = ARR_DIMS(query_arr)[0];

    return zvec_search_internal(fcinfo,
                                 col_name, qdata, qlen, topk,
                                 NULL /* no filter */);
}

PG_FUNCTION_INFO_V1(zvec_search_filtered);
Datum
zvec_search_filtered(PG_FUNCTION_ARGS)
{
    char       *col_name   = text_to_cstring(PG_GETARG_TEXT_PP(0));
    ArrayType  *query_arr  = PG_GETARG_ARRAYTYPE_P(1);
    int         topk       = PG_GETARG_INT32(2);
    char       *filter     = text_to_cstring(PG_GETARG_TEXT_PP(3));
    float4     *qdata;
    int         qlen;

    if (ARR_NDIM(query_arr) != 1)
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("query must be a 1-D float4 array")));
    if (ARR_ELEMTYPE(query_arr) != FLOAT4OID)
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("query must be float4[]")));
    if (ARR_HASNULL(query_arr))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("query vector must not contain NULLs")));

    qdata = (float4 *) ARR_DATA_PTR(query_arr);
    qlen  = ARR_DIMS(query_arr)[0];

    return zvec_search_internal(fcinfo,
                                 col_name, qdata, qlen, topk, filter);
}

/* ----------------------------------------------------------------
 * zvec_attach_table(collection_name, table_name, pk_col, vec_col)
 *
 * Installs an AFTER INSERT OR UPDATE OR DELETE trigger on table_name
 * that keeps the zvec collection in sync.
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(zvec_attach_table);
Datum
zvec_attach_table(PG_FUNCTION_ARGS)
{
    char *col_name  = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *tbl_name  = text_to_cstring(PG_GETARG_TEXT_PP(1));
    char *pk_col    = text_to_cstring(PG_GETARG_TEXT_PP(2));
    char *vec_col   = text_to_cstring(PG_GETARG_TEXT_PP(3));

    char         trig_name[ZVEC_MAX_NAME_LEN + 12];
    StringInfoData sql;
    int          spi_rc;

    snprintf(trig_name, sizeof(trig_name), "zvec_sync_%s", col_name);

    initStringInfo(&sql);
    appendStringInfo(&sql,
        "CREATE TRIGGER %s "
        "AFTER INSERT OR UPDATE OR DELETE ON %s "
        "FOR EACH ROW EXECUTE FUNCTION zvec_sync_trigger(%s, %s, %s)",
        quote_identifier(trig_name),
        quote_identifier(tbl_name),
        quote_literal_cstr(col_name),
        quote_literal_cstr(pk_col),
        quote_literal_cstr(vec_col));

    SPI_connect();
    spi_rc = SPI_execute(sql.data, false, 0);
    SPI_finish();

    if (spi_rc != SPI_OK_UTILITY)
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("zvec_attach_table: failed to create trigger"),
                 errdetail("SPI_execute returned %d", spi_rc)));

    PG_RETURN_VOID();
}

/* ----------------------------------------------------------------
 * zvec_sync_trigger()  –  AFTER INSERT OR UPDATE OR DELETE trigger
 *
 * Trigger arguments (tgargs): collection_name, pk_column, vec_column
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(zvec_sync_trigger);
Datum
zvec_sync_trigger(PG_FUNCTION_ARGS)
{
    TriggerData *trigdata = (TriggerData *) fcinfo->context;
    Trigger     *trigger;
    Relation     rel;
    TupleDesc    tupdesc;
    HeapTuple    old_tup, new_tup;
    bool         is_insert, is_update, is_delete;
    const char  *col_name, *pk_col, *vec_col;
    int          pk_attnum, vec_attnum;
    int          i;

    /* Validate trigger context */
    if (!CALLED_AS_TRIGGER(fcinfo))
        ereport(ERROR,
                (errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
                 errmsg("zvec_sync_trigger: not called as trigger")));
    if (!TRIGGER_FIRED_AFTER(trigdata->tg_event))
        ereport(ERROR,
                (errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
                 errmsg("zvec_sync_trigger: must be an AFTER trigger")));
    if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
        ereport(ERROR,
                (errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
                 errmsg("zvec_sync_trigger: must be FOR EACH ROW")));

    trigger = trigdata->tg_trigger;
    if (trigger->tgnargs != 3)
        ereport(ERROR,
                (errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
                 errmsg("zvec_sync_trigger: requires exactly 3 arguments "
                        "(collection_name, pk_column, vec_column)")));

    col_name = trigger->tgargs[0];
    pk_col   = trigger->tgargs[1];
    vec_col  = trigger->tgargs[2];

    rel     = trigdata->tg_relation;
    tupdesc = RelationGetDescr(rel);
    old_tup = trigdata->tg_trigtuple;
    new_tup = trigdata->tg_newtuple;   /* NULL for INSERT/DELETE */

    is_insert = TRIGGER_FIRED_BY_INSERT(trigdata->tg_event);
    is_update = TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event);
    is_delete = TRIGGER_FIRED_BY_DELETE(trigdata->tg_event);
    (void) is_insert;   /* INSERT is the fallthrough case */
    (void) is_update;   /* used implicitly: !is_delete */

    /* Find attribute numbers for pk and vector columns */
    pk_attnum  = -1;
    vec_attnum = -1;
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        if (attr->attisdropped)
            continue;
        if (strcmp(NameStr(attr->attname), pk_col) == 0)
            pk_attnum = attr->attnum;
        if (strcmp(NameStr(attr->attname), vec_col) == 0)
            vec_attnum = attr->attnum;
    }

    if (pk_attnum < 0)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("zvec_sync_trigger: PK column \"%s\" not found",
                        pk_col)));
    if (vec_attnum < 0)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("zvec_sync_trigger: vector column \"%s\" not found",
                        vec_col)));

    /* --- Extract PK value as a string --- */
    {
        HeapTuple  pk_tuple = is_delete ? old_tup
                                        : (new_tup ? new_tup : old_tup);
        bool       pk_isnull;
        Datum      pk_datum;
        Oid        pk_typid;
        Oid        pk_outfunc;
        bool       pk_isvarlena;
        char      *pk_str;

        pk_datum = heap_getattr(pk_tuple, pk_attnum, tupdesc, &pk_isnull);
        if (pk_isnull)
            return PointerGetDatum(NULL);   /* skip NULL PK */

        pk_typid = TupleDescAttr(tupdesc, pk_attnum - 1)->atttypid;
        getTypeOutputInfo(pk_typid, &pk_outfunc, &pk_isvarlena);
        pk_str = OidOutputFunctionCall(pk_outfunc, pk_datum);

        if (is_delete)
        {
            /* DELETE: send ZVEC_REQ_DELETE */
            ZvecRequest  req;
            ZvecResponse resp;
            char         errbuf[256];
            int          pos = 0;

            memset(&req, 0, sizeof(req));
            req.type        = ZVEC_REQ_DELETE;
            req.database_id = MyDatabaseId;

            pos = zvec_pack_str(req.data, pos, sizeof(req.data), col_name);
            if (pos < 0) goto pack_overflow;
            pos = zvec_pack_str(req.data, pos, sizeof(req.data), pk_str);
            if (pos < 0) goto pack_overflow;
            req.data_len = pos;

            if (!send_worker_request(&req, &resp, 5000, errbuf, sizeof(errbuf)))
                ereport(WARNING,
                        (errmsg("zvec_sync_trigger DELETE failed: %s",
                                errbuf)));
        }
        else
        {
            /* INSERT / UPDATE: extract vector, send ZVEC_REQ_INSERT */
            HeapTuple  vec_tuple = new_tup ? new_tup : old_tup;
            bool       vec_isnull;
            Datum      vec_datum;
            ArrayType *arr;
            float4    *vec_data;
            int        vec_len;
            ZvecRequest  req;
            ZvecResponse resp;
            char         errbuf[256];
            int          pos = 0;

            vec_datum = heap_getattr(vec_tuple, vec_attnum, tupdesc,
                                     &vec_isnull);
            if (vec_isnull)
                return PointerGetDatum(NULL);   /* skip NULL vector */

            arr = DatumGetArrayTypeP(vec_datum);

            if (ARR_ELEMTYPE(arr) != FLOAT4OID)
                ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg("zvec_sync_trigger: vector column \"%s\" "
                                "must be float4[]", vec_col)));
            if (ARR_NDIM(arr) != 1)
                ereport(ERROR,
                        (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                         errmsg("zvec_sync_trigger: vector must be 1-D")));
            if (ARR_HASNULL(arr))
                ereport(ERROR,
                        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                         errmsg("zvec_sync_trigger: vector must not contain NULLs")));

            vec_data = (float4 *) ARR_DATA_PTR(arr);
            vec_len  = ARR_DIMS(arr)[0];

            memset(&req, 0, sizeof(req));
            req.type        = ZVEC_REQ_INSERT;
            req.database_id = MyDatabaseId;

            pos = zvec_pack_str(req.data, pos, sizeof(req.data), col_name);
            if (pos < 0) goto pack_overflow;
            pos = zvec_pack_str(req.data, pos, sizeof(req.data), pk_str);
            if (pos < 0) goto pack_overflow;
            pos = zvec_pack_int(req.data, pos, sizeof(req.data), vec_len);
            if (pos < 0) goto pack_overflow;
            pos = zvec_pack_floats(req.data, pos, sizeof(req.data),
                                    vec_data, vec_len);
            if (pos < 0) goto pack_overflow;
            req.data_len = pos;

            if (!send_worker_request(&req, &resp, 5000, errbuf, sizeof(errbuf)))
                ereport(WARNING,
                        (errmsg("zvec_sync_trigger INSERT/UPDATE failed: %s",
                                errbuf)));
        }
    }

    return PointerGetDatum(NULL);

pack_overflow:
    ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("zvec_sync_trigger: request buffer overflow "
                    "(vector too large for IPC slot)")));
    return PointerGetDatum(NULL);   /* unreachable */
}

/* ----------------------------------------------------------------
 * zvec_stats(collection_name) → TABLE(key text, value text)
 *
 * Opens a read-only collection handle and returns basic statistics.
 * ---------------------------------------------------------------- */
typedef struct
{
    char key[64];
    char val[256];
} StatRow;

PG_FUNCTION_INFO_V1(zvec_stats);
Datum
zvec_stats(PG_FUNCTION_ARGS)
{
    char           *col_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    FuncCallContext *funcctx;
    StatRow         *rows;

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext         oldctx;
        TupleDesc             tupdesc;
        ZvecCollectionEntry  *entry = NULL;
        char                  data_dir[ZVEC_MAX_PATH_LEN];
        char                  errbuf[256];
        ZvecCollectionHandle *h;
        int                   doc_count;

        funcctx = SRF_FIRSTCALL_INIT();
        oldctx  = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        tupdesc = CreateTemplateTupleDesc(2);
        TupleDescInitEntry(tupdesc, (AttrNumber) 1, "key",   TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 2, "value", TEXTOID, -1, 0);
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);

        if (!pg_zvec_state)
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("pg_zvec shared memory not available")));

        /* Look up collection */
        LWLockAcquire(pg_zvec_state->lock, LW_SHARED);
        for (int i = 0; i < ZVEC_MAX_COLLECTIONS; i++)
        {
            if (pg_zvec_state->collections[i].in_use &&
                pg_zvec_state->collections[i].database_id == MyDatabaseId &&
                strcmp(pg_zvec_state->collections[i].name, col_name) == 0)
            {
                strlcpy(data_dir,
                        pg_zvec_state->collections[i].data_dir,
                        sizeof(data_dir));
                entry = &pg_zvec_state->collections[i];
                break;
            }
        }
        LWLockRelease(pg_zvec_state->lock);

        if (!entry)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_OBJECT),
                     errmsg("zvec collection \"%s\" not found", col_name)));

        h = zvec_collection_open(data_dir, true, errbuf, sizeof(errbuf));
        if (!h)
            ereport(ERROR,
                    (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                     errmsg("zvec_stats: cannot open collection: %s", errbuf)));

        doc_count = zvec_collection_doc_count(h);
        zvec_collection_close(h);

        rows = (StatRow *) palloc(2 * sizeof(StatRow));
        snprintf(rows[0].key, sizeof(rows[0].key), "collection_name");
        snprintf(rows[0].val, sizeof(rows[0].val), "%s", col_name);
        snprintf(rows[1].key, sizeof(rows[1].key), "doc_count");
        snprintf(rows[1].val, sizeof(rows[1].val), "%d", doc_count);

        funcctx->user_fctx = rows;
        funcctx->max_calls = 2;

        MemoryContextSwitchTo(oldctx);
    }

    funcctx = SRF_PERCALL_SETUP();

    if (funcctx->call_cntr < funcctx->max_calls)
    {
        StatRow   *rows = (StatRow *) funcctx->user_fctx;
        int        i    = (int) funcctx->call_cntr;
        Datum      values[2];
        bool       nulls[2] = {false, false};
        HeapTuple  tuple;

        values[0] = CStringGetTextDatum(rows[i].key);
        values[1] = CStringGetTextDatum(rows[i].val);
        tuple     = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }

    SRF_RETURN_DONE(funcctx);
}

/* ----------------------------------------------------------------
 * zvec_worker_status() → TABLE(key text, value text)
 *
 * Returns live worker state from shared memory.
 * ---------------------------------------------------------------- */
typedef struct
{
    char key[64];
    char val[256];
} StatusRow;

PG_FUNCTION_INFO_V1(zvec_worker_status);
Datum
zvec_worker_status(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    StatusRow       *rows;

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext oldctx;
        TupleDesc     tupdesc;

        funcctx = SRF_FIRSTCALL_INIT();
        oldctx  = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        tupdesc = CreateTemplateTupleDesc(2);
        TupleDescInitEntry(tupdesc, (AttrNumber) 1, "key",   TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 2, "value", TEXTOID, -1, 0);
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);

        rows = (StatusRow *) palloc(3 * sizeof(StatusRow));

        snprintf(rows[0].key, sizeof(rows[0].key), "worker_pid");
        snprintf(rows[0].val, sizeof(rows[0].val), "%d",
                 pg_zvec_state ? (int) pg_zvec_state->worker_pid : -1);

        snprintf(rows[1].key, sizeof(rows[1].key), "worker_ready");
        snprintf(rows[1].val, sizeof(rows[1].val), "%s",
                 (pg_zvec_state && pg_zvec_state->worker_ready) ? "true" : "false");

        snprintf(rows[2].key, sizeof(rows[2].key), "num_collections");
        snprintf(rows[2].val, sizeof(rows[2].val), "%d",
                 pg_zvec_state ? pg_zvec_state->num_collections : 0);

        funcctx->user_fctx = rows;
        funcctx->max_calls = 3;

        MemoryContextSwitchTo(oldctx);
    }

    funcctx = SRF_PERCALL_SETUP();

    if (funcctx->call_cntr < funcctx->max_calls)
    {
        StatusRow  *rows  = (StatusRow *) funcctx->user_fctx;
        int         i     = (int) funcctx->call_cntr;
        Datum       values[2];
        bool        nulls[2] = {false, false};
        HeapTuple   tuple;

        values[0] = CStringGetTextDatum(rows[i].key);
        values[1] = CStringGetTextDatum(rows[i].val);
        tuple     = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }

    SRF_RETURN_DONE(funcctx);
}
