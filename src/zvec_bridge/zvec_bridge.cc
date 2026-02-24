/*
 * zvec_bridge.cc
 *
 * C++ implementation of the C-callable bridge to zvec.
 *
 * Compile guard: define USE_ZVEC (via CMake / Makefile) once the zvec
 * static library and its headers are available.  Without USE_ZVEC every
 * function returns a stub error so the extension still loads and all
 * infrastructure (GUCs, shared memory, background worker, SQL functions)
 * can be exercised.
 */

#include "zvec_bridge.h"

#include <cstring>
#include <cstdio>

#ifdef USE_ZVEC
/* ----------------------------------------------------------------
 * Real implementation (compiled only when zvec is available)
 * ---------------------------------------------------------------- */
#include <zvec/db/collection.h>
#include <zvec/db/doc.h>
#include <zvec/db/index_params.h>
#include <zvec/db/options.h>
#include <zvec/db/query_params.h>
#include <zvec/db/schema.h>
#include <zvec/db/type.h>

/* Thin struct that wraps the shared_ptr so we can pass it through a C API */
struct ZvecCollectionHandle
{
    zvec::Collection::Ptr col;
};

static zvec::MetricType
parse_metric(const char *metric)
{
    if (strcmp(metric, "l2")     == 0) return zvec::MetricType::L2;
    if (strcmp(metric, "ip")     == 0) return zvec::MetricType::IP;
    if (strcmp(metric, "cosine") == 0) return zvec::MetricType::COSINE;
    return zvec::MetricType::L2;        /* default */
}

static zvec::IndexType
parse_index_type(const char *index_type)
{
    if (strcmp(index_type, "hnsw") == 0) return zvec::IndexType::HNSW;
    if (strcmp(index_type, "ivf")  == 0) return zvec::IndexType::IVF;
    if (strcmp(index_type, "flat") == 0) return zvec::IndexType::FLAT;
    return zvec::IndexType::HNSW;       /* default */
}

extern "C" {

ZvecCollectionHandle *
zvec_collection_create(const char *data_dir,
                       const char *index_type,
                       const char *metric,
                       int         dimension,
                       const char * /* params_json – TODO: parse */,
                       char       *errbuf,
                       int         errbuf_len)
{
    auto metric_type = parse_metric(metric);
    auto idx_type    = parse_index_type(index_type);
    (void)idx_type; /* TODO: use idx_type to select HNSW/IVF/FLAT */

    /* Build schema with a single vector field named "embedding" */
    zvec::CollectionSchema schema("default");
    auto vec_field = std::make_shared<zvec::FieldSchema>(
        "embedding",
        zvec::DataType::VECTOR_FP32,
        static_cast<uint32_t>(dimension),
        /*nullable=*/false,
        std::make_shared<zvec::HnswIndexParams>(metric_type));
    schema.add_field(vec_field);

    /* Also add a string primary-key forward field */
    auto pk_field = std::make_shared<zvec::FieldSchema>(
        "__pk__", zvec::DataType::STRING, /*nullable=*/false);
    schema.add_field(pk_field);

    zvec::CollectionOptions opts;
    auto result = zvec::Collection::CreateAndOpen(data_dir, schema, opts);
    if (!result)
    {
        snprintf(errbuf, errbuf_len, "%s", result.error().message().c_str());
        return nullptr;
    }

    auto *h = new ZvecCollectionHandle();
    h->col  = std::move(*result);
    return h;
}

ZvecCollectionHandle *
zvec_collection_open(const char *data_dir,
                     bool        read_only,
                     char       *errbuf,
                     int         errbuf_len)
{
    zvec::CollectionOptions opts;
    opts.read_only_   = read_only;
    opts.enable_mmap_ = true;

    auto result = zvec::Collection::Open(data_dir, opts);
    if (!result)
    {
        snprintf(errbuf, errbuf_len, "%s", result.error().message().c_str());
        return nullptr;
    }

    auto *h = new ZvecCollectionHandle();
    h->col  = std::move(*result);
    return h;
}

bool
zvec_collection_flush(ZvecCollectionHandle *h, char *errbuf, int errbuf_len)
{
    auto st = h->col->Flush();
    if (!st.ok())
    {
        snprintf(errbuf, errbuf_len, "%s", st.message().c_str());
        return false;
    }
    return true;
}

void
zvec_collection_close(ZvecCollectionHandle *h)
{
    delete h;
}

bool
zvec_collection_destroy(ZvecCollectionHandle *h, char *errbuf, int errbuf_len)
{
    auto st = h->col->Destroy();
    if (!st.ok())
    {
        snprintf(errbuf, errbuf_len, "%s", st.message().c_str());
        return false;
    }
    /* Do NOT delete h here; caller must call zvec_collection_close() to free. */
    return true;
}

bool
zvec_collection_upsert(ZvecCollectionHandle *h,
                       const char  *pk,
                       const float *vec,
                       int          vec_len,
                       char        *errbuf,
                       int          errbuf_len)
{
    zvec::Doc doc;
    doc.set_pk(pk);
    doc.set(std::string("embedding"),
            std::vector<float>(vec, vec + vec_len));
    doc.set(std::string("__pk__"), std::string(pk));

    std::vector<zvec::Doc> docs = {doc};
    auto result = h->col->Upsert(docs);
    if (!result)
    {
        snprintf(errbuf, errbuf_len, "%s", result.error().message().c_str());
        return false;
    }
    return true;
}

bool
zvec_collection_delete(ZvecCollectionHandle *h,
                       const char *pk,
                       char       *errbuf,
                       int         errbuf_len)
{
    auto result = h->col->Delete({std::string(pk)});
    if (!result)
    {
        snprintf(errbuf, errbuf_len, "%s", result.error().message().c_str());
        return false;
    }
    return true;
}

bool
zvec_collection_optimize(ZvecCollectionHandle *h, char *errbuf, int errbuf_len)
{
    auto st = h->col->Optimize();
    if (!st.ok())
    {
        snprintf(errbuf, errbuf_len, "%s", st.message().c_str());
        return false;
    }
    return true;
}

int
zvec_collection_search(ZvecCollectionHandle *h,
                       const float          *query,
                       int                   query_len,
                       int                   topk,
                       const char           *filter_expr,
                       ZvecSearchResult     *out_results,
                       char                 *errbuf,
                       int                   errbuf_len)
{
    zvec::VectorQuery vq;
    vq.topk_       = topk;
    vq.field_name_ = "embedding";
    vq.query_vector_.assign(reinterpret_cast<const char *>(query),
                            query_len * sizeof(float));
    if (filter_expr)
        vq.filter_ = filter_expr;

    auto result = h->col->Query(vq);
    if (!result)
    {
        snprintf(errbuf, errbuf_len, "%s", result.error().message().c_str());
        return -1;
    }

    const auto &docs = *result;
    int n = static_cast<int>(docs.size());
    for (int i = 0; i < n && i < topk; ++i)
    {
        snprintf(out_results[i].pk, sizeof(out_results[i].pk),
                 "%s", docs[i]->pk().c_str());
        out_results[i].score = docs[i]->score();
    }
    return n;
}

int
zvec_collection_scan_all(ZvecCollectionHandle *h,
                          int                   max_rows,
                          int                   dimension,
                          char                (*out_pks)[256],
                          float                *out_vecs,
                          char                 *errbuf,
                          int                   errbuf_len)
{
    /* kMaxQueryTopk inside zvec is 1024; clamp silently */
    if (max_rows > 1024)
        max_rows = 1024;

    /*
     * Step 1: forward scan without include_vector_ to get PKs.
     *
     * include_vector_=true fails for the writing segment because its
     * combined vector indexer is not built until Optimize().  Use
     * include_vector_=false here (the forward store still returns all
     * scalar fields including __pk__ and the Doc::pk()).
     */
    zvec::VectorQuery vq;
    vq.topk_           = max_rows;
    vq.field_name_     = "";    /* no vector field → forward scan */
    vq.query_vector_   = "";    /* empty → not an ANN query */
    vq.include_vector_ = false; /* avoid writing-segment vector indexer */

    auto scan_result = h->col->Query(vq);
    if (!scan_result)
    {
        snprintf(errbuf, errbuf_len, "%s", scan_result.error().message().c_str());
        return -1;
    }

    const auto &scan_docs = *scan_result;
    int n = static_cast<int>(scan_docs.size());
    if (n > max_rows)
        n = max_rows;

    if (n == 0)
        return 0;

    /* Collect PKs and copy them to the output buffer */
    std::vector<std::string> pk_list;
    pk_list.reserve(n);
    for (int i = 0; i < n; ++i)
    {
        const std::string &pk = scan_docs[i]->pk();
        snprintf(out_pks[i], 256, "%s", pk.c_str());
        pk_list.push_back(pk);
    }

    /*
     * Step 2: fetch full docs (including vector data) from the forward
     * store via Fetch().  This path reads directly from the Parquet/Arrow
     * forward store and does not touch the vector indexer, so it works
     * even before Optimize() has been called.
     */
    auto fetch_result = h->col->Fetch(pk_list);
    if (!fetch_result)
    {
        snprintf(errbuf, errbuf_len, "%s", fetch_result.error().message().c_str());
        return -1;
    }

    const auto &doc_map = *fetch_result;   /* unordered_map<string, Doc::Ptr> */

    for (int i = 0; i < n; ++i)
    {
        float *vec_slot = out_vecs + (size_t)i * dimension;
        auto it = doc_map.find(pk_list[i]);
        if (it != doc_map.end() && it->second)
        {
            auto vec_opt = it->second->get<std::vector<float>>("embedding");
            if (vec_opt.has_value())
            {
                const auto &vec = vec_opt.value();
                int copy_len = std::min(static_cast<int>(vec.size()), dimension);
                std::memcpy(vec_slot, vec.data(), copy_len * sizeof(float));
                if (copy_len < dimension)
                    std::memset(vec_slot + copy_len, 0,
                                (dimension - copy_len) * sizeof(float));
                continue;
            }
        }
        /* pk not found in fetch result or no embedding field — zero-fill */
        std::memset(vec_slot, 0, dimension * sizeof(float));
    }
    return n;
}

int
zvec_collection_doc_count(ZvecCollectionHandle *h)
{
    auto result = h->col->Stats();
    if (!result)
        return -1;
    return static_cast<int>(result->doc_count);
}

} /* extern "C" */

#else /* !USE_ZVEC */
/* ----------------------------------------------------------------
 * Stub implementation: the extension loads but every bridge call
 * returns an informative error.
 * ---------------------------------------------------------------- */

struct ZvecCollectionHandle { int dummy; };

static void
stub_err(char *errbuf, int errbuf_len, const char *fn)
{
    snprintf(errbuf, errbuf_len,
             "%s: zvec library not compiled in "
             "(rebuild with -DUSE_ZVEC=ON)", fn);
}

extern "C" {

ZvecCollectionHandle *
zvec_collection_create(const char *, const char *, const char *, int,
                       const char *, char *errbuf, int errbuf_len)
{ stub_err(errbuf, errbuf_len, "zvec_collection_create"); return nullptr; }

ZvecCollectionHandle *
zvec_collection_open(const char *, bool, char *errbuf, int errbuf_len)
{ stub_err(errbuf, errbuf_len, "zvec_collection_open"); return nullptr; }

bool zvec_collection_flush(ZvecCollectionHandle *, char *errbuf, int errbuf_len)
{ stub_err(errbuf, errbuf_len, "zvec_collection_flush"); return false; }

void zvec_collection_close(ZvecCollectionHandle *) {}

bool zvec_collection_destroy(ZvecCollectionHandle *, char *errbuf, int errbuf_len)
{ stub_err(errbuf, errbuf_len, "zvec_collection_destroy"); return false; }

bool zvec_collection_upsert(ZvecCollectionHandle *, const char *, const float *,
                             int, char *errbuf, int errbuf_len)
{ stub_err(errbuf, errbuf_len, "zvec_collection_upsert"); return false; }

bool zvec_collection_delete(ZvecCollectionHandle *, const char *,
                             char *errbuf, int errbuf_len)
{ stub_err(errbuf, errbuf_len, "zvec_collection_delete"); return false; }

bool zvec_collection_optimize(ZvecCollectionHandle *, char *errbuf, int errbuf_len)
{ stub_err(errbuf, errbuf_len, "zvec_collection_optimize"); return false; }

int zvec_collection_search(ZvecCollectionHandle *, const float *, int, int,
                            const char *, ZvecSearchResult *,
                            char *errbuf, int errbuf_len)
{ stub_err(errbuf, errbuf_len, "zvec_collection_search"); return -1; }

int zvec_collection_scan_all(ZvecCollectionHandle *, int, int,
                              char (*)[256], float *,
                              char * /*errbuf*/, int /*errbuf_len*/)
{ return 0; /* no rows — zvec library not compiled in */ }

int zvec_collection_doc_count(ZvecCollectionHandle *) { return -1; }

} /* extern "C" */

#endif /* USE_ZVEC */
