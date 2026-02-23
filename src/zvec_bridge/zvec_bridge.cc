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
    delete h;
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
zvec_collection_doc_count(ZvecCollectionHandle *h)
{
    auto result = h->col->Stats();
    if (!result)
        return -1;
    /* Stats returns a struct; total_doc_count is the aggregate */
    return static_cast<int>(result->total_doc_count_);
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

int zvec_collection_doc_count(ZvecCollectionHandle *) { return -1; }

} /* extern "C" */

#endif /* USE_ZVEC */
