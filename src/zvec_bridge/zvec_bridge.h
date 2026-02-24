/*
 * zvec_bridge.h
 *
 * C-callable wrapper around zvec's C++ API.
 * Compiled as C++ (zvec_bridge.cc) and linked into the extension .so.
 *
 * When USE_ZVEC is not defined the functions return error codes / NULL so
 * that the rest of the extension still compiles and loads without a built
 * zvec library.
 */
#ifndef ZVEC_BRIDGE_H
#define ZVEC_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle for a zvec Collection */
typedef struct ZvecCollectionHandle ZvecCollectionHandle;

/* ----------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------- */

/*
 * Create a new collection at `data_dir`.
 * index_type: "hnsw" | "ivf" | "flat"
 * metric:     "l2" | "ip" | "cosine"
 * dimension:  vector dimension
 * params_json: optional extra parameters as JSON string (may be NULL)
 *
 * Returns NULL on failure; sets errbuf.
 */
ZvecCollectionHandle *zvec_collection_create(const char *data_dir,
                                              const char *index_type,
                                              const char *metric,
                                              int         dimension,
                                              const char *params_json,
                                              char       *errbuf,
                                              int         errbuf_len);

/*
 * Open an existing collection.
 * read_only: true → multiple backends may open concurrently (search only).
 */
ZvecCollectionHandle *zvec_collection_open(const char *data_dir,
                                            bool        read_only,
                                            char       *errbuf,
                                            int         errbuf_len);

/* Flush pending writes to disk. */
bool zvec_collection_flush(ZvecCollectionHandle *h,
                            char *errbuf, int errbuf_len);

/* Close and free the handle. */
void zvec_collection_close(ZvecCollectionHandle *h);

/* Permanently delete the collection's data directory. */
bool zvec_collection_destroy(ZvecCollectionHandle *h,
                              char *errbuf, int errbuf_len);

/* ----------------------------------------------------------------
 * Write operations (must be done through the worker handle)
 * ---------------------------------------------------------------- */

/*
 * Insert or upsert a dense FP32 vector.
 * pk:       string primary key
 * vec:      pointer to float32 array
 * vec_len:  number of elements (must match collection dimension)
 */
bool zvec_collection_upsert(ZvecCollectionHandle *h,
                             const char  *pk,
                             const float *vec,
                             int          vec_len,
                             char        *errbuf,
                             int          errbuf_len);

/* Delete a document by primary key. */
bool zvec_collection_delete(ZvecCollectionHandle *h,
                             const char *pk,
                             char       *errbuf,
                             int         errbuf_len);

/* Trigger segment merge + ANN index build. */
bool zvec_collection_optimize(ZvecCollectionHandle *h,
                               char *errbuf, int errbuf_len);

/* ----------------------------------------------------------------
 * Search (can be called from read-only backend handles)
 * ---------------------------------------------------------------- */

typedef struct ZvecSearchResult
{
    char    pk[256];
    float   score;
} ZvecSearchResult;

/*
 * Perform ANN search.
 * query:        float32 query vector
 * query_len:    dimension
 * topk:         number of results requested
 * filter_expr:  optional SQL-style scalar filter (NULL = no filter)
 * out_results:  caller-allocated array of at least `topk` entries
 *
 * Returns the number of results actually written (≤ topk), or -1 on error.
 */
int zvec_collection_search(ZvecCollectionHandle    *h,
                            const float             *query,
                            int                      query_len,
                            int                      topk,
                            const char              *filter_expr,
                            ZvecSearchResult        *out_results,
                            char                    *errbuf,
                            int                      errbuf_len);

/* ----------------------------------------------------------------
 * Full-table scan (no vector query)
 * ---------------------------------------------------------------- */

/* Maximum rows returned by a single zvec_collection_scan_all call. */
#define ZVEC_SCAN_MAX_ROWS 1024

/*
 * Scan all documents without a vector query (full forward scan).
 *
 * out_pks  must point to a char[max_rows][256] buffer.
 * out_vecs must point to a float[max_rows * dimension] buffer.
 *
 * Returns the number of rows written (≤ max_rows), or -1 on error.
 */
int zvec_collection_scan_all(ZvecCollectionHandle *h,
                              int                   max_rows,
                              int                   dimension,
                              char                (*out_pks)[256],
                              float                *out_vecs,
                              char                 *errbuf,
                              int                   errbuf_len);

/* ----------------------------------------------------------------
 * Metadata
 * ---------------------------------------------------------------- */
int zvec_collection_doc_count(ZvecCollectionHandle *h);

#ifdef __cplusplus
}
#endif

#endif /* ZVEC_BRIDGE_H */
