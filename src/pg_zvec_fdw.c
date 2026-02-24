/*
 * pg_zvec_fdw.c
 *
 * Foreign Data Wrapper for zvec vector collections.
 *
 * Phase 1 scope:
 *   - Register FDW handler / validator
 *   - CREATE FOREIGN TABLE  → create zvec Collection via worker IPC
 *   - DROP   FOREIGN TABLE  → destroy zvec Collection via worker IPC
 *   - Stub scan / modify callbacks (return errors; replaced in Phase 2-3)
 */

#include "pg_zvec_fdw.h"
#include "pg_zvec_shmem.h"
#include "zvec_bridge/zvec_bridge.h"

#include <stdio.h>

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "catalog/namespace.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "optimizer/appendinfo.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "tcop/utility.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC;

/* ----------------------------------------------------------------
 * GUC parameters
 * ---------------------------------------------------------------- */
char *pg_zvec_data_dir      = NULL;
int   pg_zvec_query_threads    = 4;
int   pg_zvec_optimize_threads = 2;
int   pg_zvec_max_buffer_mb    = 64;

/* ----------------------------------------------------------------
 * Hook chains
 * ---------------------------------------------------------------- */
static shmem_startup_hook_type      prev_shmem_startup_hook    = NULL;
static ProcessUtility_hook_type     prev_process_utility_hook  = NULL;
#if PG_VERSION_NUM >= 150000
static shmem_request_hook_type      prev_shmem_request_hook    = NULL;
#endif

/* ----------------------------------------------------------------
 * Helpers: options parsing
 * ---------------------------------------------------------------- */

/*
 * zvec_get_vec_type_for_column — read the "type" column option for a given
 * attno.  Returns ZVEC_VEC_FP32 by default if no option is specified.
 */
static int
zvec_get_vec_type_for_column(Oid relid, AttrNumber attno)
{
    List     *options;
    ListCell *lc;

    options = GetForeignColumnOptions(relid, attno);
    foreach(lc, options)
    {
        DefElem *def = (DefElem *) lfirst(lc);
        if (strcmp(def->defname, "type") == 0)
        {
            const char *v = defGetString(def);
            if (strcasecmp(v, "vector_fp16") == 0)
                return ZVEC_VEC_FP16;
            else
                return ZVEC_VEC_FP32;
        }
    }
    return ZVEC_VEC_FP32; /* default */
}

/*
 * zvec_table_options — extract zvec-specific OPTIONS from a ForeignTable.
 *
 * All output pointers are palloc'd strings or default values.
 */
static void
zvec_table_options(ForeignTable *ft, ForeignServer *server,
                   char **out_data_dir,
                   int  *out_dimension,
                   char **out_metric,
                   char **out_index_type,
                   char **out_params_json)
{
    ListCell *lc;
    char     *data_dir    = NULL;
    int       dimension   = 0;
    char     *metric      = pstrdup("cosine");
    char     *index_type  = pstrdup("hnsw");
    /* Build params JSON from optional m / ef_construction / nlist */
    int       m = 0, efc = 0, nlist = 0;

    /* Server-level options */
    foreach(lc, server->options)
    {
        DefElem *def = (DefElem *) lfirst(lc);
        if (strcmp(def->defname, "data_dir") == 0)
            data_dir = defGetString(def);
    }

    /* Table-level options override server-level */
    foreach(lc, ft->options)
    {
        DefElem *def = (DefElem *) lfirst(lc);
        if (strcmp(def->defname, "dimension") == 0)
            dimension = atoi(defGetString(def));
        else if (strcmp(def->defname, "metric") == 0)
            metric = defGetString(def);
        else if (strcmp(def->defname, "index_type") == 0)
            index_type = defGetString(def);
        else if (strcmp(def->defname, "m") == 0)
            m = atoi(defGetString(def));
        else if (strcmp(def->defname, "ef_construction") == 0)
            efc = atoi(defGetString(def));
        else if (strcmp(def->defname, "nlist") == 0)
            nlist = atoi(defGetString(def));
    }

    /* Build params JSON */
    {
    StringInfoData buf;
    bool first = true;
    initStringInfo(&buf);
    appendStringInfoChar(&buf, '{');
    if (m > 0)
    {
        appendStringInfo(&buf, "\"m\":%d", m);
        first = false;
    }
    if (efc > 0)
    {
        if (!first) appendStringInfoChar(&buf, ',');
        appendStringInfo(&buf, "\"ef_construction\":%d", efc);
        first = false;
    }
    if (nlist > 0)
    {
        if (!first) appendStringInfoChar(&buf, ',');
        appendStringInfo(&buf, "\"nlist\":%d", nlist);
    }
    appendStringInfoChar(&buf, '}');
    *out_params_json = buf.data;
    }

    /* Compute data_dir */
    if (!data_dir || data_dir[0] == '\0')
    {
        if (pg_zvec_data_dir && pg_zvec_data_dir[0] != '\0')
            data_dir = pg_zvec_data_dir;
        else
        {
            StringInfoData d;
            initStringInfo(&d);
            appendStringInfo(&d, "%s/pg_zvec", DataDir);
            data_dir = d.data;
        }
    }

    *out_data_dir   = data_dir;
    *out_dimension  = dimension;
    *out_metric     = metric;
    *out_index_type = index_type;
}

/* ----------------------------------------------------------------
 * IPC: send a request to the background worker and wait for response
 * ---------------------------------------------------------------- */
static bool
send_worker_request(ZvecRequest *req, ZvecResponse *resp,
                    int timeout_ms, char *errbuf, int errbuf_len)
{
    int waited = 0;

    if (!pg_zvec_state)
    {
        snprintf(errbuf, errbuf_len, "pg_zvec shared memory not initialised");
        return false;
    }

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
        snprintf(errbuf, errbuf_len, "pg_zvec worker is busy; try again later");
        return false;
    }

    req->sender_pid = MyProcPid;
    memcpy(&pg_zvec_state->request, req, sizeof(ZvecRequest));
    pg_zvec_state->request_pending = true;
    pg_zvec_state->response_ready  = false;

    if (pg_zvec_state->worker_pid > 0)
    {
        PGPROC *worker = BackendPidGetProc(pg_zvec_state->worker_pid);
        if (worker)
            SetLatch(&worker->procLatch);
    }

    LWLockRelease(pg_zvec_state->lock);

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

/* ----------------------------------------------------------------
 * DDL helpers: create / drop a collection for a foreign table
 * ---------------------------------------------------------------- */

static void
zvec_create_collection_for_table(Oid relid)
{
    ForeignTable  *ft;
    ForeignServer *server;
    char          *data_dir;
    int            dimension;
    char          *metric;
    char          *index_type;
    char          *params_json;
    char           col_path[ZVEC_MAX_PATH_LEN];
    char           col_name[ZVEC_MAX_NAME_LEN];
    char           errbuf[256];
    ZvecRequest    req;
    ZvecResponse   resp;
    int            pos = 0;
    int            vec_attno = -1;
    int            vec_type  = ZVEC_VEC_FP32;
    TupleDesc      tupdesc;
    int            i;

    ft     = GetForeignTable(relid);
    server = GetForeignServer(ft->serverid);

    zvec_table_options(ft, server,
                       &data_dir, &dimension, &metric, &index_type, &params_json);

    if (dimension <= 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pg_zvec: foreign table must specify a positive \"dimension\" option")));

    /* Locate first float4[] column to read its vec_type option */
    tupdesc = RelationGetDescr(RelationIdGetRelation(relid));
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        if (!attr->attisdropped && attr->atttypid == FLOAT4ARRAYOID)
        {
            List     *col_options;
            ListCell *lc;

            vec_attno = i + 1;  /* 1-based */

            /* Validate column-level options */
            col_options = GetForeignColumnOptions(relid, vec_attno);
            foreach(lc, col_options)
            {
                DefElem *def = (DefElem *) lfirst(lc);
                if (strcmp(def->defname, "type") == 0)
                {
                    const char *v = defGetString(def);
                    if (strcasecmp(v, "vector_fp32") != 0 &&
                        strcasecmp(v, "vector_fp16") != 0)
                        ereport(ERROR,
                                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                 errmsg("pg_zvec: column option \"type\" must be vector_fp32 or vector_fp16")));
                    if (strcasecmp(v, "vector_fp16") == 0)
                        vec_type = ZVEC_VEC_FP16;
                }
                else
                {
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("pg_zvec: unrecognised column option \"%s\"",
                                    def->defname)));
                }
            }
            break;
        }
    }

    /* Collection name = relation name (schema-unqualified) */
    strlcpy(col_name, get_rel_name(relid), sizeof(col_name));

    /* Full path = data_dir / col_name */
    snprintf(col_path, sizeof(col_path), "%s/%s", data_dir, col_name);

    memset(&req, 0, sizeof(req));
    req.type        = ZVEC_REQ_CREATE_COLLECTION;
    req.database_id = MyDatabaseId;

    pos = zvec_pack_str(req.data, pos, sizeof(req.data), col_name);
    if (pos < 0) goto overflow;
    pos = zvec_pack_str(req.data, pos, sizeof(req.data), col_path);
    if (pos < 0) goto overflow;
    pos = zvec_pack_str(req.data, pos, sizeof(req.data), index_type);
    if (pos < 0) goto overflow;
    pos = zvec_pack_str(req.data, pos, sizeof(req.data), metric);
    if (pos < 0) goto overflow;
    pos = zvec_pack_int(req.data, pos, sizeof(req.data), dimension);
    if (pos < 0) goto overflow;
    pos = zvec_pack_str(req.data, pos, sizeof(req.data),
                        (vec_type == ZVEC_VEC_FP16) ? "vector_fp16" : "vector_fp32");
    if (pos < 0) goto overflow;
    pos = zvec_pack_str(req.data, pos, sizeof(req.data), params_json);
    if (pos < 0) goto overflow;
    req.data_len = pos;

    if (!send_worker_request(&req, &resp, 10000, errbuf, sizeof(errbuf)))
        ereport(WARNING,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pg_zvec: failed to create collection \"%s\": %s",
                        col_name, errbuf),
                 errhint("The foreign table metadata was recorded; "
                         "the zvec collection will be retried on next access.")));
    return;

overflow:
    ereport(WARNING,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("pg_zvec: IPC buffer overflow building CREATE request")));
}

static void
zvec_drop_collection_for_table(Oid relid)
{
    char          col_name[ZVEC_MAX_NAME_LEN];
    char          errbuf[256];
    ZvecRequest   req;
    ZvecResponse  resp;
    int           pos = 0;
    char         *rel_name;

    rel_name = get_rel_name(relid);
    if (!rel_name)
        return; /* already gone */

    strlcpy(col_name, rel_name, sizeof(col_name));

    memset(&req, 0, sizeof(req));
    req.type        = ZVEC_REQ_DROP_COLLECTION;
    req.database_id = MyDatabaseId;

    pos = zvec_pack_str(req.data, pos, sizeof(req.data), col_name);
    if (pos < 0)
    {
        ereport(WARNING,
                (errmsg("pg_zvec: IPC buffer overflow building DROP request")));
        return;
    }
    req.data_len = pos;

    if (!send_worker_request(&req, &resp, 10000, errbuf, sizeof(errbuf)))
        ereport(WARNING,
                (errmsg("pg_zvec: failed to drop collection \"%s\": %s",
                        col_name, errbuf)));
}

/* ----------------------------------------------------------------
 * ProcessUtility_hook: intercept CREATE / DROP FOREIGN TABLE
 *
 * CREATE: run after standard_ProcessUtility so the pg_foreign_table
 *         row exists and we can look up options.
 * DROP:   run before standard_ProcessUtility so the catalog is intact
 *         and we can look up collection names.
 * ---------------------------------------------------------------- */

/*
 * is_zvec_foreign_table — return true if relid belongs to a foreign table
 * that uses zvec_fdw.  Returns false (not error) if the relation is not a
 * foreign table or uses a different FDW.
 */
static bool
is_zvec_foreign_table(Oid relid)
{
    ForeignTable       *ft;
    ForeignServer      *server;
    ForeignDataWrapper *fdw;

    if (get_rel_relkind(relid) != RELKIND_FOREIGN_TABLE)
        return false;

    ft     = GetForeignTable(relid);
    server = GetForeignServer(ft->serverid);
    fdw    = GetForeignDataWrapper(server->fdwid);

    return (strcmp(fdw->fdwname, "zvec_fdw") == 0);
}

static void
pg_zvec_process_utility(PlannedStmt *pstmt,
                         const char *queryString,
                         bool readOnlyTree,
                         ProcessUtilityContext context,
                         ParamListInfo params,
                         QueryEnvironment *queryEnv,
                         DestReceiver *dest,
                         QueryCompletion *qc)
{
    Node *parsetree = pstmt->utilityStmt;

    /* ----------------------------------------------------------------
     * CREATE FOREIGN TABLE: validate column options BEFORE creation
     * ---------------------------------------------------------------- */
    if (IsA(parsetree, CreateForeignTableStmt))
    {
        CreateForeignTableStmt *cfts = (CreateForeignTableStmt *) parsetree;
        ForeignServer          *server = NULL;
        ForeignDataWrapper     *fdw    = NULL;
        ListCell               *lc;

        /* Check if this is a zvec_fdw table */
        if (cfts->servername)
        {
            server = GetForeignServerByName(cfts->servername, true);
            if (server)
            {
                fdw = GetForeignDataWrapper(server->fdwid);
                if (fdw && strcmp(fdw->fdwname, "zvec_fdw") == 0)
                {
                    /* Validate column-level options for zvec tables */
                    foreach(lc, cfts->base.tableElts)
                    {
                        Node *node = (Node *) lfirst(lc);
                        if (IsA(node, ColumnDef))
                        {
                            ColumnDef *colDef = (ColumnDef *) node;
                            ListCell  *opt_lc;

                            foreach(opt_lc, colDef->fdwoptions)
                            {
                                DefElem *def = (DefElem *) lfirst(opt_lc);
                                if (strcmp(def->defname, "type") == 0)
                                {
                                    const char *v = defGetString(def);
                                    if (strcasecmp(v, "vector_fp32") != 0 &&
                                        strcasecmp(v, "vector_fp16") != 0)
                                        ereport(ERROR,
                                                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                                 errmsg("pg_zvec: column option \"type\" must be vector_fp32 or vector_fp16")));
                                }
                                else
                                {
                                    ereport(ERROR,
                                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                             errmsg("pg_zvec: unrecognised column option \"%s\"",
                                                    def->defname)));
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* ----------------------------------------------------------------
     * DROP FOREIGN TABLE: collect collection names BEFORE the drop so
     * the catalog is still intact.
     * ---------------------------------------------------------------- */
    if (IsA(parsetree, DropStmt))
    {
        DropStmt *drop = (DropStmt *) parsetree;

        if (drop->removeType == OBJECT_FOREIGN_TABLE)
        {
            ListCell *lc;
            /* Collect OIDs now; catalog will be gone after standard_ProcessUtility */
            List *drop_oids = NIL;

            foreach(lc, drop->objects)
            {
                RangeVar *rv  = makeRangeVarFromNameList((List *) lfirst(lc));
                Oid       oid = RangeVarGetRelid(rv, AccessShareLock, true);

                if (OidIsValid(oid) && is_zvec_foreign_table(oid))
                    drop_oids = lappend_oid(drop_oids, oid);
            }

            /* Fire IPC for each collection BEFORE the catalog entry disappears */
            {
                ListCell *lc2;
                foreach(lc2, drop_oids)
                    zvec_drop_collection_for_table(lfirst_oid(lc2));
            }
        }
    }

    /* ----------------------------------------------------------------
     * Run standard (or chained) ProcessUtility
     * ---------------------------------------------------------------- */
    if (prev_process_utility_hook)
        prev_process_utility_hook(pstmt, queryString, readOnlyTree,
                                   context, params, queryEnv, dest, qc);
    else
        standard_ProcessUtility(pstmt, queryString, readOnlyTree,
                                 context, params, queryEnv, dest, qc);

    /* ----------------------------------------------------------------
     * CREATE FOREIGN TABLE: create the collection AFTER the table
     * (and its pg_foreign_table row) has been fully inserted.
     * ---------------------------------------------------------------- */
    if (IsA(parsetree, CreateForeignTableStmt))
    {
        CreateForeignTableStmt *cfts = (CreateForeignTableStmt *) parsetree;
        RangeVar *rv  = cfts->base.relation;
        Oid       oid = RangeVarGetRelid(rv, NoLock, true);

        if (OidIsValid(oid) && is_zvec_foreign_table(oid))
            zvec_create_collection_for_table(oid);
    }
}

/* ----------------------------------------------------------------
 * FdwRoutine callbacks — Phase 1 stubs
 * ---------------------------------------------------------------- */

static void
zvec_get_foreign_rel_size(PlannerInfo *root,
                           RelOptInfo  *baserel,
                           Oid          foreigntableid)
{
    baserel->rows = 1000;
}

static void
zvec_get_foreign_paths(PlannerInfo *root,
                        RelOptInfo  *baserel,
                        Oid          foreigntableid)
{
    add_path(baserel,
             (Path *) create_foreignscan_path(root, baserel,
                                               NULL,            /* default pathtarget */
                                               baserel->rows,
                                               1,               /* startup cost */
                                               baserel->rows * 1,
                                               NIL,             /* no pathkeys */
                                               baserel->lateral_relids,
                                               NULL,
                                               NIL));
}

static ForeignScan *
zvec_get_foreign_plan(PlannerInfo      *root,
                       RelOptInfo       *baserel,
                       Oid               foreigntableid,
                       ForeignPath      *best_path,
                       List             *tlist,
                       List             *scan_clauses,
                       Plan             *outer_plan)
{
    scan_clauses = extract_actual_clauses(scan_clauses, false);
    return make_foreignscan(tlist,
                             scan_clauses,
                             baserel->relid,
                             NIL,  /* no remote exprs */
                             best_path->fdw_private,
                             NIL,  /* no custom tlist */
                             NIL,  /* no remote conds */
                             outer_plan);
}

static void
zvec_begin_foreign_scan(ForeignScanState *node, int eflags)
{
    Relation             rel     = node->ss.ss_currentRelation;
    TupleDesc            tupdesc = RelationGetDescr(rel);
    ForeignTable        *ft;
    ForeignServer       *server;
    char                *data_dir, *metric, *index_type, *params_json;
    int                  dimension;
    ZvecFdwScanState    *state;
    char                 col_name[ZVEC_MAX_NAME_LEN];
    char                 errbuf[256];
    Oid                  pk_typinput;
    int                  i;

    state = palloc0(sizeof(ZvecFdwScanState));

    if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
    {
        state->done = true;
        node->fdw_state = state;
        return;
    }

    /* Get FDW options */
    ft     = GetForeignTable(RelationGetRelid(rel));
    server = GetForeignServer(ft->serverid);
    zvec_table_options(ft, server, &data_dir, &dimension,
                       &metric, &index_type, &params_json);
    state->dimension = dimension;
    state->natts     = tupdesc->natts;

    /* Locate PK column (attno 1) */
    state->pk_attno     = 1;
    state->pk_atttypmod = TupleDescAttr(tupdesc, 0)->atttypmod;
    getTypeInputInfo(TupleDescAttr(tupdesc, 0)->atttypid,
                     &pk_typinput, &state->pk_typioparam);
    fmgr_info(pk_typinput, &state->pk_finfo);

    /* Locate first float4[] column */
    state->vec_attno = -1;
    state->vec_type  = ZVEC_VEC_FP32;
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        if (!attr->attisdropped && attr->atttypid == FLOAT4ARRAYOID)
        {
            state->vec_attno = i + 1;   /* 1-based */
            state->vec_type  = zvec_get_vec_type_for_column(RelationGetRelid(rel),
                                                             state->vec_attno);
            break;
        }
    }

    if (state->vec_attno < 0 || dimension <= 0)
    {
        state->done = true;
        node->fdw_state = state;
        return;
    }

    /* ----------------------------------------------------------------
     * Route the scan through the background worker.
     *
     * zvec uses an exclusive file lock for write handles.  The worker
     * owns the write handle, so we cannot open a read-only handle
     * concurrently.  Instead we send ZVEC_REQ_SCAN; the worker calls
     * zvec_collection_scan_all() and writes the results to a tmpfile.
     * ---------------------------------------------------------------- */
    strlcpy(col_name, RelationGetRelationName(rel), sizeof(col_name));

    {
        ZvecRequest  req;
        ZvecResponse resp;
        char         tmpfile[ZVEC_MAX_PATH_LEN];
        int          max_rows = ZVEC_SCAN_MAX_ROWS;
        int          pos = 0;
        FILE        *fp;
        int          nrows = 0;
        char       (*pks)[256] = NULL;
        float       *vecs = NULL;

        snprintf(tmpfile, sizeof(tmpfile),
                 "/tmp/pg_zvec_scan_%d.bin", MyProcPid);

        memset(&req, 0, sizeof(req));
        req.type        = ZVEC_REQ_SCAN;
        req.database_id = MyDatabaseId;

        pos = zvec_pack_str(req.data, pos, sizeof(req.data), col_name);
        if (pos < 0) goto overflow;
        pos = zvec_pack_int(req.data, pos, sizeof(req.data), max_rows);
        if (pos < 0) goto overflow;
        pos = zvec_pack_int(req.data, pos, sizeof(req.data), dimension);
        if (pos < 0) goto overflow;
        pos = zvec_pack_str(req.data, pos, sizeof(req.data), tmpfile);
        if (pos < 0) goto overflow;
        req.data_len = pos;

        if (!send_worker_request(&req, &resp, 30000, errbuf, sizeof(errbuf)))
        {
            ereport(WARNING,
                    (errmsg("pg_zvec: scan request failed for \"%s\": %s",
                            col_name, errbuf)));
            state->done = true;
            node->fdw_state = state;
            return;
        }

        /* Read results from tmpfile */
        fp = fopen(tmpfile, "rb");
        if (fp)
        {
            if (fread(&nrows, sizeof(int), 1, fp) == 1 && nrows > 0)
            {
                pks  = (char (*)[256]) palloc(nrows * 256);
                vecs = (float *) palloc((Size)((int64) nrows * dimension
                                               * (int64) sizeof(float)));
                if (fread(pks, 256, nrows, fp) != (size_t) nrows ||
                    fread(vecs, sizeof(float) * dimension, nrows, fp) != (size_t) nrows)
                {
                    /* Partial read — treat as empty */
                    pfree(pks);
                    pfree(vecs);
                    pks   = NULL;
                    vecs  = NULL;
                    nrows = 0;
                }
            }
            fclose(fp);
            unlink(tmpfile);
        }
        else
        {
            ereport(WARNING,
                    (errmsg("pg_zvec: could not open scan results for \"%s\"",
                            col_name)));
        }

        state->nrows = nrows;
        state->pks   = pks;
        state->vecs  = vecs;
        state->done  = (nrows == 0);
    }

    node->fdw_state = state;
    return;

overflow:
    ereport(WARNING,
            (errmsg("pg_zvec: IPC buffer overflow building SCAN request")));
    state->done = true;
    node->fdw_state = state;
}

static TupleTableSlot *
zvec_iterate_foreign_scan(ForeignScanState *node)
{
    ZvecFdwScanState *state  = (ZvecFdwScanState *) node->fdw_state;
    TupleTableSlot   *slot   = node->ss.ss_ScanTupleSlot;
    TupleDesc         tupdesc = slot->tts_tupleDescriptor;
    int               natts  = tupdesc->natts;
    int               i;

    if (state->done || state->cur >= state->nrows)
        return ExecClearTuple(slot);

    ExecClearTuple(slot);

    /* Default all columns to NULL */
    for (i = 0; i < natts; i++)
    {
        slot->tts_values[i] = (Datum) 0;
        slot->tts_isnull[i] = true;
    }

    /* Fill PK column (attno 1) */
    {
        char  *pk_str   = state->pks[state->cur];
        Datum  pk_datum = InputFunctionCall(&state->pk_finfo,
                                            pk_str,
                                            state->pk_typioparam,
                                            state->pk_atttypmod);
        slot->tts_values[state->pk_attno - 1] = pk_datum;
        slot->tts_isnull[state->pk_attno - 1] = false;
    }

    /* Fill vector column (first float4[]) */
    if (state->vec_attno >= 1 && state->vec_attno <= natts)
    {
        float     *vec   = state->vecs + (int64) state->cur * state->dimension;
        int        dim   = state->dimension;
        Datum     *elems = (Datum *) palloc(dim * sizeof(Datum));
        int        dims[1] = {dim};
        int        lbs[1]  = {1};
        ArrayType *arr;

        for (i = 0; i < dim; i++)
            elems[i] = Float4GetDatum(vec[i]);

        arr = construct_md_array(elems, NULL, 1, dims, lbs,
                                 FLOAT4OID, sizeof(float4), true, TYPALIGN_INT);
        pfree(elems);

        slot->tts_values[state->vec_attno - 1] = PointerGetDatum(arr);
        slot->tts_isnull[state->vec_attno - 1] = false;
    }

    ExecStoreVirtualTuple(slot);
    state->cur++;
    return slot;
}

static void
zvec_rescan_foreign_scan(ForeignScanState *node)
{
    ZvecFdwScanState *state = (ZvecFdwScanState *) node->fdw_state;
    if (state)
        state->cur = 0;
}

static void
zvec_end_foreign_scan(ForeignScanState *node)
{
    ZvecFdwScanState *state = (ZvecFdwScanState *) node->fdw_state;
    if (state)
    {
        if (state->pks)  pfree(state->pks);
        if (state->vecs) pfree(state->vecs);
        pfree(state);
        node->fdw_state = NULL;
    }
}

/* ----------------------------------------------------------------
 * FdwRoutine callbacks — Phase 3: INSERT / DELETE
 * ---------------------------------------------------------------- */

/*
 * zvec_add_foreign_update_targets
 *
 * Register the PK column (attno 1) as a junk "zvec_pk" attribute so the
 * executor can pass it to ExecForeignDelete.
 */
static void
zvec_add_foreign_update_targets(PlannerInfo *root,
                                 Index rtindex,
                                 RangeTblEntry *target_rte,
                                 Relation target_relation)
{
    Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(target_relation), 0);
    Var *var = makeVar(rtindex,
                       1,               /* attno of pk column (1-based) */
                       attr->atttypid,
                       attr->atttypmod,
                       attr->attcollation,
                       0);              /* varlevelsup */
    add_row_identity_var(root, var, rtindex, "zvec_pk");
}

/*
 * zvec_plan_foreign_modify — no planner-side work needed.
 */
static List *
zvec_plan_foreign_modify(PlannerInfo *root,
                          ModifyTable *plan,
                          Index resultRelation,
                          int subplan_index)
{
    return NIL;
}

/*
 * zvec_begin_foreign_modify
 *
 * Set up ZvecFdwModifyState: locate pk and vector columns, look up the
 * junk attribute number for DELETE.
 */
static void
zvec_begin_foreign_modify(ModifyTableState *mtstate,
                           ResultRelInfo *rinfo,
                           List *fdw_private,
                           int subplan_index,
                           int eflags)
{
    ZvecFdwModifyState *mstate;
    Relation            rel = rinfo->ri_RelationDesc;
    TupleDesc           tupdesc = RelationGetDescr(rel);
    ForeignTable       *ft;
    ForeignServer      *server;
    char               *data_dir, *metric, *index_type, *params_json;
    int                 dimension;
    int                 i;

    if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
        return;

    mstate = palloc0(sizeof(ZvecFdwModifyState));

    /* Collection name = relation name (schema-unqualified) */
    strlcpy(mstate->collection_name, RelationGetRelationName(rel),
            sizeof(mstate->collection_name));

    /* Get dimension from FDW options */
    ft     = GetForeignTable(RelationGetRelid(rel));
    server = GetForeignServer(ft->serverid);
    zvec_table_options(ft, server, &data_dir, &dimension,
                       &metric, &index_type, &params_json);
    mstate->dimension = dimension;

    /* PK column: attno 1 by convention; look up its output function so we
     * can convert any type (int, bigint, uuid, text, …) to a cstring. */
    mstate->pk_attno = 1;
    getTypeOutputInfo(TupleDescAttr(tupdesc, 0)->atttypid,
                      &mstate->pk_typoutput,
                      &mstate->pk_typisvarlena);

    /* Vector column: first float4[] attribute */
    mstate->vec_attno = -1;
    mstate->vec_type  = ZVEC_VEC_FP32;
    for (i = 0; i < tupdesc->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
        if (!attr->attisdropped && attr->atttypid == FLOAT4ARRAYOID)
        {
            mstate->vec_attno = i + 1;  /* 1-based */
            mstate->vec_type  = zvec_get_vec_type_for_column(RelationGetRelid(rel),
                                                              mstate->vec_attno);
            break;
        }
    }
    if (mstate->vec_attno < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                 errmsg("pg_zvec: no float4[] vector column found in \"%s\"",
                        mstate->collection_name)));

    /* For DELETE: locate the "zvec_pk" junk attribute in the subplan */
    if (mtstate->operation == CMD_DELETE)
    {
        Plan *subplan = outerPlanState(mtstate)->plan;
        mstate->pk_junk_attno =
            ExecFindJunkAttributeInTlist(subplan->targetlist, "zvec_pk");
        if (!AttributeNumberIsValid(mstate->pk_junk_attno))
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("pg_zvec: could not find junk attribute \"zvec_pk\"")));
    }

    rinfo->ri_FdwState = mstate;
}

/*
 * zvec_exec_foreign_insert
 *
 * Extract pk (attno 1) and vector (first float4[]) from the slot and send a
 * ZVEC_REQ_INSERT to the background worker.
 */
static TupleTableSlot *
zvec_exec_foreign_insert(EState *estate,
                          ResultRelInfo *rinfo,
                          TupleTableSlot *slot,
                          TupleTableSlot *planSlot)
{
    ZvecFdwModifyState *mstate = (ZvecFdwModifyState *) rinfo->ri_FdwState;
    ZvecRequest         req;
    ZvecResponse        resp;
    char                errbuf[256];
    int                 pos = 0;
    bool                isnull;
    Datum               pk_datum;
    Datum               vec_datum;
    char               *pk_str;
    ArrayType          *arr;
    float4             *vec;
    int                 vec_len;

    /* Extract PK (text) */
    slot_getallattrs(slot);
    pk_datum = slot_getattr(slot, mstate->pk_attno, &isnull);
    if (isnull)
        ereport(ERROR,
                (errcode(ERRCODE_NOT_NULL_VIOLATION),
                 errmsg("pg_zvec: pk column (attno %d) must not be NULL",
                        mstate->pk_attno)));
    pk_str = OidOutputFunctionCall(mstate->pk_typoutput, pk_datum);
    if (strlen(pk_str) >= 256)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("pg_zvec: pk value exceeds maximum length of 255 bytes")));

    /* Extract vector (float4[]) */
    vec_datum = slot_getattr(slot, mstate->vec_attno, &isnull);
    if (isnull)
        ereport(ERROR,
                (errcode(ERRCODE_NOT_NULL_VIOLATION),
                 errmsg("pg_zvec: vector column must not be NULL")));
    arr = DatumGetArrayTypeP(vec_datum);
    if (ARR_NDIM(arr) != 1)
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("pg_zvec: vector must be a 1-dimensional float4 array")));
    vec_len = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
    if (vec_len != mstate->dimension)
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("pg_zvec: expected vector dimension %d, got %d",
                        mstate->dimension, vec_len)));
    vec = (float4 *) ARR_DATA_PTR(arr);

    /* Build IPC payload: [name\0][pk\0][vec_len:int32][float32*vec_len] */
    memset(&req, 0, sizeof(req));
    req.type        = ZVEC_REQ_INSERT;
    req.database_id = MyDatabaseId;

    pos = zvec_pack_str(req.data, pos, sizeof(req.data), mstate->collection_name);
    if (pos < 0) goto overflow;
    pos = zvec_pack_str(req.data, pos, sizeof(req.data), pk_str);
    if (pos < 0) goto overflow;
    pos = zvec_pack_int(req.data, pos, sizeof(req.data), vec_len);
    if (pos < 0) goto overflow;
    pos = zvec_pack_floats(req.data, pos, sizeof(req.data), vec, vec_len);
    if (pos < 0) goto overflow;
    req.data_len = pos;

    if (!send_worker_request(&req, &resp, 10000, errbuf, sizeof(errbuf)))
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pg_zvec: INSERT failed: %s", errbuf)));

    return slot;

overflow:
    ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("pg_zvec: IPC buffer overflow building INSERT request")));
    return NULL; /* unreachable */
}

/*
 * zvec_exec_foreign_delete
 *
 * Extract the pk from the "zvec_pk" junk attribute in planSlot and send a
 * ZVEC_REQ_DELETE to the background worker.
 */
static TupleTableSlot *
zvec_exec_foreign_delete(EState *estate,
                          ResultRelInfo *rinfo,
                          TupleTableSlot *slot,
                          TupleTableSlot *planSlot)
{
    ZvecFdwModifyState *mstate = (ZvecFdwModifyState *) rinfo->ri_FdwState;
    ZvecRequest         req;
    ZvecResponse        resp;
    char                errbuf[256];
    int                 pos = 0;
    bool                isnull;
    Datum               pk_datum;
    char               *pk_str;

    /* Get PK from junk attribute in planSlot */
    pk_datum = ExecGetJunkAttribute(planSlot, mstate->pk_junk_attno, &isnull);
    if (isnull)
        ereport(ERROR,
                (errcode(ERRCODE_NOT_NULL_VIOLATION),
                 errmsg("pg_zvec: junk pk attribute is NULL")));
    pk_str = OidOutputFunctionCall(mstate->pk_typoutput, pk_datum);

    /* Build IPC payload: [name\0][pk\0] */
    memset(&req, 0, sizeof(req));
    req.type        = ZVEC_REQ_DELETE;
    req.database_id = MyDatabaseId;

    pos = zvec_pack_str(req.data, pos, sizeof(req.data), mstate->collection_name);
    if (pos < 0) goto overflow;
    pos = zvec_pack_str(req.data, pos, sizeof(req.data), pk_str);
    if (pos < 0) goto overflow;
    req.data_len = pos;

    if (!send_worker_request(&req, &resp, 10000, errbuf, sizeof(errbuf)))
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("pg_zvec: DELETE failed: %s", errbuf)));

    return slot;

overflow:
    ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("pg_zvec: IPC buffer overflow building DELETE request")));
    return NULL; /* unreachable */
}

/*
 * zvec_end_foreign_modify — release per-statement state.
 */
static void
zvec_end_foreign_modify(EState *estate, ResultRelInfo *rinfo)
{
    ZvecFdwModifyState *mstate = (ZvecFdwModifyState *) rinfo->ri_FdwState;
    if (mstate)
        pfree(mstate);
}

/* ----------------------------------------------------------------
 * FDW handler
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(zvec_fdw_handler);
Datum
zvec_fdw_handler(PG_FUNCTION_ARGS)
{
    FdwRoutine *routine = makeNode(FdwRoutine);

    /* Scan */
    routine->GetForeignRelSize  = zvec_get_foreign_rel_size;
    routine->GetForeignPaths    = zvec_get_foreign_paths;
    routine->GetForeignPlan     = zvec_get_foreign_plan;
    routine->BeginForeignScan   = zvec_begin_foreign_scan;
    routine->IterateForeignScan = zvec_iterate_foreign_scan;
    routine->ReScanForeignScan  = zvec_rescan_foreign_scan;
    routine->EndForeignScan     = zvec_end_foreign_scan;

    /* Modify (Phase 3) */
    routine->AddForeignUpdateTargets = zvec_add_foreign_update_targets;
    routine->PlanForeignModify       = zvec_plan_foreign_modify;
    routine->BeginForeignModify      = zvec_begin_foreign_modify;
    routine->ExecForeignInsert       = zvec_exec_foreign_insert;
    routine->ExecForeignDelete       = zvec_exec_foreign_delete;
    routine->EndForeignModify        = zvec_end_foreign_modify;

    PG_RETURN_POINTER(routine);
}

/* ----------------------------------------------------------------
 * OPTIONS validator
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(zvec_fdw_validator);
Datum
zvec_fdw_validator(PG_FUNCTION_ARGS)
{
    List     *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
    Oid       catalog       = PG_GETARG_OID(1);
    ListCell *lc;
    bool      has_dimension = false;

    foreach(lc, options_list)
    {
        DefElem *def = (DefElem *) lfirst(lc);

        if (catalog == ForeignTableRelationId)
        {
            if (strcmp(def->defname, "dimension") == 0)
            {
                int dim = atoi(defGetString(def));
                if (dim <= 0)
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("pg_zvec: \"dimension\" must be a positive integer")));
                has_dimension = true;
            }
            else if (strcmp(def->defname, "metric") == 0)
            {
                const char *v = defGetString(def);
                if (strcmp(v, "l2")     != 0 &&
                    strcmp(v, "ip")     != 0 &&
                    strcmp(v, "cosine") != 0)
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("pg_zvec: \"metric\" must be l2, ip, or cosine")));
            }
            else if (strcmp(def->defname, "index_type") == 0)
            {
                const char *v = defGetString(def);
                if (strcmp(v, "hnsw") != 0 &&
                    strcmp(v, "ivf")  != 0 &&
                    strcmp(v, "flat") != 0)
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("pg_zvec: \"index_type\" must be hnsw, ivf, or flat")));
            }
            else if (strcmp(def->defname, "m")              != 0 &&
                     strcmp(def->defname, "ef_construction") != 0 &&
                     strcmp(def->defname, "nlist")           != 0)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("pg_zvec: unrecognised foreign table option \"%s\"",
                                def->defname)));
            }
        }
        else if (catalog == ForeignServerRelationId)
        {
            if (strcmp(def->defname, "data_dir") != 0)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("pg_zvec: unrecognised server option \"%s\"",
                                def->defname)));
        }
        else if (catalog == AttributeRelationId)
        {
            /* Column-level options: only "type" is allowed */
            if (strcmp(def->defname, "type") == 0)
            {
                const char *v = defGetString(def);
                if (strcasecmp(v, "vector_fp32") != 0 &&
                    strcasecmp(v, "vector_fp16") != 0)
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                             errmsg("pg_zvec: column option \"type\" must be vector_fp32 or vector_fp16")));
            }
            else
            {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("pg_zvec: unrecognised column option \"%s\"",
                                def->defname)));
            }
        }
    }

    if (catalog == ForeignTableRelationId && !has_dimension)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pg_zvec: foreign table must specify \"dimension\" option")));

    PG_RETURN_VOID();
}

/* ----------------------------------------------------------------
 * Shared memory request hook (PG15+)
 * ---------------------------------------------------------------- */
#if PG_VERSION_NUM >= 150000
static void
pg_zvec_shmem_request_hook(void)
{
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();
    RequestAddinShmemSpace(pg_zvec_shmem_size());
    RequestNamedLWLockTranche("pg_zvec", 1);
}
#endif

/* ----------------------------------------------------------------
 * Shared memory startup
 * ---------------------------------------------------------------- */
static void
pg_zvec_shmem_startup_hook(void)
{
    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();
    pg_zvec_shmem_startup();
}

/* ----------------------------------------------------------------
 * Background worker registration
 * ---------------------------------------------------------------- */
static void
register_background_worker(void)
{
    BackgroundWorker worker;

    MemSet(&worker, 0, sizeof(worker));
    worker.bgw_flags        = BGWORKER_SHMEM_ACCESS;
    worker.bgw_start_time   = BgWorkerStart_ConsistentState;
    worker.bgw_restart_time = 5;

    snprintf(worker.bgw_library_name,  BGW_MAXLEN, "pg_zvec");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "pg_zvec_worker_main");
    snprintf(worker.bgw_name,          BGW_MAXLEN, "pg_zvec worker");
    snprintf(worker.bgw_type,          BGW_MAXLEN, "pg_zvec");

    worker.bgw_notify_pid = 0;
    RegisterBackgroundWorker(&worker);
}

/* ----------------------------------------------------------------
 * _PG_init
 * ---------------------------------------------------------------- */
void _PG_init(void);

void
_PG_init(void)
{
    if (!process_shared_preload_libraries_in_progress)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_zvec must be loaded via shared_preload_libraries")));

    /* GUCs */
    DefineCustomStringVariable(
        "pg_zvec.data_dir",
        "Directory where zvec collections are stored.",
        "Empty string defaults to $PGDATA/pg_zvec.",
        &pg_zvec_data_dir,
        "",
        PGC_POSTMASTER,
        0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "pg_zvec.query_threads",
        "Number of threads used for vector search queries.",
        NULL,
        &pg_zvec_query_threads,
        4, 1, 64,
        PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "pg_zvec.optimize_threads",
        "Number of threads used for index optimization.",
        NULL,
        &pg_zvec_optimize_threads,
        2, 1, 32,
        PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "pg_zvec.max_buffer_mb",
        "Maximum write-buffer size per collection in MiB.",
        NULL,
        &pg_zvec_max_buffer_mb,
        64, 8, 8192,
        PGC_SIGHUP, 0, NULL, NULL, NULL);

    /* Shared memory */
#if PG_VERSION_NUM >= 150000
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook      = pg_zvec_shmem_request_hook;
#else
    RequestAddinShmemSpace(pg_zvec_shmem_size());
    RequestNamedLWLockTranche("pg_zvec", 1);
#endif
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook      = pg_zvec_shmem_startup_hook;

    /* Background worker */
    register_background_worker();

    /* DDL hook */
    prev_process_utility_hook = ProcessUtility_hook;
    ProcessUtility_hook       = pg_zvec_process_utility;
}
