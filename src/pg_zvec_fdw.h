/*
 * pg_zvec_fdw.h
 *
 * Internal declarations shared between the FDW source files.
 */
#ifndef PG_ZVEC_FDW_H
#define PG_ZVEC_FDW_H

#include "postgres.h"
#include "access/attnum.h"
#include "fmgr.h"

/* ----------------------------------------------------------------
 * Vector storage type constants
 * ---------------------------------------------------------------- */
#define ZVEC_VEC_FP32  0
#define ZVEC_VEC_FP16  1

/* ----------------------------------------------------------------
 * GUC parameters (defined in pg_zvec_fdw.c)
 * ---------------------------------------------------------------- */
extern char *pg_zvec_data_dir;
extern int   pg_zvec_query_threads;
extern int   pg_zvec_optimize_threads;
extern int   pg_zvec_max_buffer_mb;

/* ----------------------------------------------------------------
 * FDW handler entry points (registered in pg_zvec--2.0.sql)
 * ---------------------------------------------------------------- */
PGDLLEXPORT Datum zvec_fdw_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum zvec_fdw_validator(PG_FUNCTION_ARGS);

/* ----------------------------------------------------------------
 * Distance operator functions (Phase 2)
 * ---------------------------------------------------------------- */
PGDLLEXPORT Datum zvec_l2_distance(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum zvec_cosine_distance(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum zvec_ip_distance(PG_FUNCTION_ARGS);

/* ----------------------------------------------------------------
 * Background worker entry point
 * ---------------------------------------------------------------- */
PGDLLEXPORT void pg_zvec_worker_main(Datum main_arg);

/* ----------------------------------------------------------------
 * Maximum number of scalar (non-pk, non-vector) columns tracked
 * ---------------------------------------------------------------- */
#define ZVEC_MAX_SCALAR_FIELDS  32

/* ----------------------------------------------------------------
 * Per-scan execution state
 * ---------------------------------------------------------------- */
typedef struct ZvecFdwScanState
{
    bool        done;            /* true → scan is exhausted */
    /* result buffers (palloc'd in BeginForeignScan) */
    int         nrows;           /* total rows fetched from zvec */
    int         cur;             /* next row to return (0-based) */
    int         dimension;       /* vector dimension */
    int         pk_attno;        /* 1-based attno of pk column */
    int         vec_attno;       /* 1-based attno of float4[] column */
    int         vec_type;        /* ZVEC_VEC_FP32 or ZVEC_VEC_FP16 */
    int         natts;           /* number of table attributes */
    char      (*pks)[256];       /* [nrows][256] pk strings */
    float      *vecs;            /* [nrows * dimension] float32 values */
    /* for converting pk string → Datum */
    Oid         pk_typioparam;
    int32       pk_atttypmod;
    FmgrInfo    pk_finfo;        /* pg input function for pk type */

    /* Scalar field support */
    int         n_scalar_fields;                          /* count of scalar columns */
    int         scalar_attno[ZVEC_MAX_SCALAR_FIELDS];     /* 1-based attno in tupdesc */
    char       *scalar_names[ZVEC_MAX_SCALAR_FIELDS];     /* zvec field names (= PG col names) */
    /* Per-row scalar values: flat array [nrows * n_scalar_fields].
     * scalar_vals[row * n_scalar_fields + fld] is NULL for null, else palloc'd string. */
    char      **scalar_vals;
    /* For converting scalar string → Datum */
    FmgrInfo    scalar_finfos[ZVEC_MAX_SCALAR_FIELDS];
    Oid         scalar_typioparams[ZVEC_MAX_SCALAR_FIELDS];
    int32       scalar_atttypmod[ZVEC_MAX_SCALAR_FIELDS];

    /* ANN query state (Phase 2) */
    bool        is_ann_scan;                              /* true if this is an ANN path */
    float      *query_vec;                                /* query vector (palloc'd) */
    int         query_vec_len;                            /* dimension */
    int         topk;                                     /* LIMIT k */
    char       *filter_expr;                              /* scalar filter expression (optional) */
} ZvecFdwScanState;

/* ----------------------------------------------------------------
 * Per-modify execution state (INSERT / DELETE)
 *
 * Convention: attno 1 (the first column) is the primary key (text).
 * The vector column is the first float4[] column found in the tuple.
 * ---------------------------------------------------------------- */
typedef struct ZvecFdwModifyState
{
    char        collection_name[128];   /* zvec collection name (= relation name) */
    int         pk_attno;               /* 1-based attno of pk column */
    int         vec_attno;              /* 1-based attno of float4[] column */
    int         vec_type;               /* ZVEC_VEC_FP32 or ZVEC_VEC_FP16 */
    int         dimension;              /* expected vector length */
    AttrNumber  pk_junk_attno;          /* junk attno in planSlot (DELETE only) */
    Oid         pk_typoutput;           /* output function OID for pk type */
    bool        pk_typisvarlena;        /* is pk type varlena? */

    /* Scalar column support for INSERT */
    int         n_scalar_fields;
    int         scalar_attno[ZVEC_MAX_SCALAR_FIELDS];      /* 1-based attno */
    char       *scalar_names[ZVEC_MAX_SCALAR_FIELDS];      /* zvec field names */
    Oid         scalar_typoutput[ZVEC_MAX_SCALAR_FIELDS];  /* output fn OID */
    bool        scalar_typisvarlena[ZVEC_MAX_SCALAR_FIELDS];
} ZvecFdwModifyState;

#endif /* PG_ZVEC_FDW_H */
