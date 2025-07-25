/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <faiss/IVFlib.h>
#include <omp.h>

#include <memory>
#include <numeric>

#include <faiss/IndexAdditiveQuantizer.h>
#include <faiss/IndexIVFAdditiveQuantizer.h>
#include <faiss/IndexIVFIndependentQuantizer.h>
#include <faiss/IndexPreTransform.h>
#include <faiss/IndexRefine.h>
#include <faiss/MetaIndexes.h>
#include <faiss/clone_index.h>
#include <faiss/impl/FaissAssert.h>
#include <faiss/index_io.h>
#include <faiss/utils/distances.h>
#include <faiss/utils/hamming.h>
#include <faiss/utils/utils.h>

namespace faiss {
namespace ivflib {

void check_compatible_for_merge(const Index* index0, const Index* index1) {
    const faiss::IndexPreTransform* pt0 =
            dynamic_cast<const faiss::IndexPreTransform*>(index0);

    if (pt0) {
        const faiss::IndexPreTransform* pt1 =
                dynamic_cast<const faiss::IndexPreTransform*>(index1);
        FAISS_THROW_IF_NOT_MSG(pt1, "both indexes should be pretransforms");

        FAISS_THROW_IF_NOT(pt0->chain.size() == pt1->chain.size());
        for (int i = 0; i < pt0->chain.size(); i++) {
            FAISS_THROW_IF_NOT(typeid(pt0->chain[i]) == typeid(pt1->chain[i]));
        }

        index0 = pt0->index;
        index1 = pt1->index;
    }
    FAISS_THROW_IF_NOT(typeid(index0) == typeid(index1));
    FAISS_THROW_IF_NOT(
            index0->d == index1->d &&
            index0->metric_type == index1->metric_type);

    const faiss::IndexIVF* ivf0 = dynamic_cast<const faiss::IndexIVF*>(index0);
    if (ivf0) {
        const faiss::IndexIVF* ivf1 =
                dynamic_cast<const faiss::IndexIVF*>(index1);
        FAISS_THROW_IF_NOT(ivf1);

        ivf0->check_compatible_for_merge(*ivf1);
    }

    // TODO: check as thoroughfully for other index types
}

const IndexIVF* try_extract_index_ivf(const Index* index) {
    auto* ivf = dynamic_cast<const IndexIVF*>(index);
    if (ivf != nullptr) {
        return ivf;
    }

    if (auto* pt = dynamic_cast<const IndexPreTransform*>(index)) {
        return try_extract_index_ivf(pt->index);
    }
    if (auto* idmap = dynamic_cast<const IndexIDMap*>(index)) {
        return try_extract_index_ivf(idmap->index);
    }
    if (auto* idmap = dynamic_cast<const IndexIDMap2*>(index)) {
        return try_extract_index_ivf(idmap->index);
    }
    if (auto* indep =
                dynamic_cast<const IndexIVFIndependentQuantizer*>(index)) {
        return try_extract_index_ivf(indep->index_ivf);
    }
    if (auto* refine = dynamic_cast<const IndexRefine*>(index)) {
        return try_extract_index_ivf(refine->base_index);
    }

    return nullptr;
}

IndexIVF* try_extract_index_ivf(Index* index) {
    return const_cast<IndexIVF*>(try_extract_index_ivf((const Index*)(index)));
}

const IndexIVF* extract_index_ivf(const Index* index) {
    const IndexIVF* ivf = try_extract_index_ivf(index);
    FAISS_THROW_IF_NOT(ivf);
    return ivf;
}

IndexIVF* extract_index_ivf(Index* index) {
    return const_cast<IndexIVF*>(extract_index_ivf((const Index*)(index)));
}

void merge_into(faiss::Index* index0, faiss::Index* index1, bool shift_ids) {
    check_compatible_for_merge(index0, index1);
    IndexIVF* ivf0 = extract_index_ivf(index0);
    IndexIVF* ivf1 = extract_index_ivf(index1);

    ivf0->merge_from(*ivf1, shift_ids ? ivf0->ntotal : 0);

    // useful for IndexPreTransform
    index0->ntotal = ivf0->ntotal;
    index1->ntotal = ivf1->ntotal;
}

void search_centroid(
        faiss::Index* index,
        const float* x,
        int n,
        idx_t* centroid_ids) {
    std::unique_ptr<float[]> del;
    if (auto index_pre = dynamic_cast<faiss::IndexPreTransform*>(index)) {
        x = index_pre->apply_chain(n, x);
        del.reset((float*)x);
        index = index_pre->index;
    }
    faiss::IndexIVF* index_ivf = dynamic_cast<faiss::IndexIVF*>(index);
    assert(index_ivf);
    index_ivf->quantizer->assign(n, x, centroid_ids);
}

void search_and_return_centroids(
        faiss::Index* index,
        size_t n,
        const float* xin,
        long k,
        float* distances,
        idx_t* labels,
        idx_t* query_centroid_ids,
        idx_t* result_centroid_ids) {
    const float* x = xin;
    std::unique_ptr<float[]> del;
    if (auto index_pre = dynamic_cast<faiss::IndexPreTransform*>(index)) {
        x = index_pre->apply_chain(n, x);
        del.reset((float*)x);
        index = index_pre->index;
    }
    faiss::IndexIVF* index_ivf = dynamic_cast<faiss::IndexIVF*>(index);
    assert(index_ivf);

    size_t nprobe = index_ivf->nprobe;
    std::vector<idx_t> cent_nos(n * nprobe);
    std::vector<float> cent_dis(n * nprobe);
    index_ivf->quantizer->search(
            n, x, nprobe, cent_dis.data(), cent_nos.data());

    if (query_centroid_ids) {
        for (size_t i = 0; i < n; i++) {
            query_centroid_ids[i] = cent_nos[i * nprobe];
        }
    }

    index_ivf->search_preassigned(
            n, x, k, cent_nos.data(), cent_dis.data(), distances, labels, true);

    for (size_t i = 0; i < n * k; i++) {
        idx_t label = labels[i];
        if (label < 0) {
            if (result_centroid_ids) {
                result_centroid_ids[i] = -1;
            }
        } else {
            long list_no = lo_listno(label);
            long list_index = lo_offset(label);
            if (result_centroid_ids) {
                result_centroid_ids[i] = list_no;
            }
            labels[i] = index_ivf->invlists->get_single_id(list_no, list_index);
        }
    }
}

SlidingIndexWindow::SlidingIndexWindow(Index* index) : index(index) {
    n_slice = 0;
    IndexIVF* index_ivf = const_cast<IndexIVF*>(extract_index_ivf(index));
    ils = dynamic_cast<ArrayInvertedLists*>(index_ivf->invlists);
    FAISS_THROW_IF_NOT_MSG(
            ils, "only supports indexes with ArrayInvertedLists");
    nlist = ils->nlist;
    sizes.resize(nlist);
}

template <class T>
static void shift_and_add(
        std::vector<T>& dst,
        size_t remove,
        const std::vector<T>& src) {
    if (remove > 0) {
        memmove(dst.data(),
                dst.data() + remove,
                (dst.size() - remove) * sizeof(T));
    }
    size_t insert_point = dst.size() - remove;
    dst.resize(insert_point + src.size());
    memcpy(dst.data() + insert_point, src.data(), src.size() * sizeof(T));
}

template <class T>
static void shift_and_add(
        MaybeOwnedVector<T>& dst,
        size_t remove,
        const MaybeOwnedVector<T>& src) {
    if (remove > 0) {
        memmove(dst.data(),
                dst.data() + remove,
                (dst.size() - remove) * sizeof(T));
    }
    size_t insert_point = dst.size() - remove;
    dst.resize(insert_point + src.size());
    memcpy(dst.data() + insert_point, src.data(), src.size() * sizeof(T));
}

template <class T>
static void remove_from_begin(std::vector<T>& v, size_t remove) {
    if (remove > 0) {
        v.erase(v.begin(), v.begin() + remove);
    }
}

template <class T>
static void remove_from_begin(MaybeOwnedVector<T>& v, size_t remove) {
    if (remove > 0) {
        v.erase(v.begin(), v.begin() + remove);
    }
}

void SlidingIndexWindow::step(const Index* sub_index, bool remove_oldest) {
    FAISS_THROW_IF_NOT_MSG(
            !remove_oldest || n_slice > 0,
            "cannot remove slice: there is none");

    const ArrayInvertedLists* ils2 = nullptr;
    if (sub_index) {
        check_compatible_for_merge(index, sub_index);
        ils2 = dynamic_cast<const ArrayInvertedLists*>(
                extract_index_ivf(sub_index)->invlists);
        FAISS_THROW_IF_NOT_MSG(ils2, "supports only ArrayInvertedLists");
    }
    IndexIVF* index_ivf = extract_index_ivf(index);

    if (remove_oldest && ils2) {
        for (int i = 0; i < nlist; i++) {
            std::vector<size_t>& sizesi = sizes[i];
            size_t amount_to_remove = sizesi[0];
            index_ivf->ntotal += ils2->ids[i].size() - amount_to_remove;

            shift_and_add(ils->ids[i], amount_to_remove, ils2->ids[i]);
            shift_and_add(
                    ils->codes[i],
                    amount_to_remove * ils->code_size,
                    ils2->codes[i]);
            for (int j = 0; j + 1 < n_slice; j++) {
                sizesi[j] = sizesi[j + 1] - amount_to_remove;
            }
            sizesi[n_slice - 1] = ils->ids[i].size();
        }
    } else if (ils2) {
        for (int i = 0; i < nlist; i++) {
            index_ivf->ntotal += ils2->ids[i].size();
            shift_and_add(ils->ids[i], 0, ils2->ids[i]);
            shift_and_add(ils->codes[i], 0, ils2->codes[i]);
            sizes[i].push_back(ils->ids[i].size());
        }
        n_slice++;
    } else if (remove_oldest) {
        for (int i = 0; i < nlist; i++) {
            size_t amount_to_remove = sizes[i][0];
            index_ivf->ntotal -= amount_to_remove;
            remove_from_begin(ils->ids[i], amount_to_remove);
            remove_from_begin(ils->codes[i], amount_to_remove * ils->code_size);
            for (int j = 0; j + 1 < n_slice; j++) {
                sizes[i][j] = sizes[i][j + 1] - amount_to_remove;
            }
            sizes[i].pop_back();
        }
        n_slice--;
    } else {
        FAISS_THROW_MSG("nothing to do???");
    }
    index->ntotal = index_ivf->ntotal;
}

// Get a subset of inverted lists [i0, i1). Works on IndexIVF's and
// IndexIVF's embedded in a IndexPreTransform

ArrayInvertedLists* get_invlist_range(const Index* index, long i0, long i1) {
    const IndexIVF* ivf = extract_index_ivf(index);

    FAISS_THROW_IF_NOT(0 <= i0 && i0 <= i1 && i1 <= ivf->nlist);

    const InvertedLists* src = ivf->invlists;

    ArrayInvertedLists* il = new ArrayInvertedLists(i1 - i0, src->code_size);

    for (long i = i0; i < i1; i++) {
        il->add_entries(
                i - i0,
                src->list_size(i),
                InvertedLists::ScopedIds(src, i).get(),
                InvertedLists::ScopedCodes(src, i).get());
    }
    return il;
}

void set_invlist_range(
        Index* index,
        long i0,
        long i1,
        ArrayInvertedLists* src) {
    IndexIVF* ivf = extract_index_ivf(index);

    FAISS_THROW_IF_NOT(0 <= i0 && i0 <= i1 && i1 <= ivf->nlist);

    ArrayInvertedLists* dst = dynamic_cast<ArrayInvertedLists*>(ivf->invlists);
    FAISS_THROW_IF_NOT_MSG(dst, "only ArrayInvertedLists supported");
    FAISS_THROW_IF_NOT(
            src->nlist == i1 - i0 && dst->code_size == src->code_size);

    size_t ntotal = index->ntotal;
    for (long i = i0; i < i1; i++) {
        ntotal -= dst->list_size(i);
        ntotal += src->list_size(i - i0);
        std::swap(src->codes[i - i0], dst->codes[i]);
        std::swap(src->ids[i - i0], dst->ids[i]);
    }
    ivf->ntotal = index->ntotal = ntotal;
}

static size_t count_ndis(
        const IndexIVF* index_ivf,
        size_t n_list_scan,
        const idx_t* Iq) {
    size_t nb_dis = 0;
    const InvertedLists* il = index_ivf->invlists;
    for (idx_t i = 0; i < n_list_scan; i++) {
        if (Iq[i] >= 0) {
            nb_dis += il->list_size(Iq[i]);
        }
    }
    return nb_dis;
}

void search_with_parameters(
        const Index* index,
        idx_t n,
        const float* x,
        idx_t k,
        float* distances,
        idx_t* labels,
        const IVFSearchParameters* params,
        size_t* nb_dis_ptr,
        double* ms_per_stage) {
    FAISS_THROW_IF_NOT(params);
    const float* prev_x = x;
    std::unique_ptr<const float[]> del;

    double t0 = getmillisecs();

    if (auto ip = dynamic_cast<const IndexPreTransform*>(index)) {
        x = ip->apply_chain(n, x);
        if (x != prev_x) {
            del.reset(x);
        }
        index = ip->index;
    }

    double t1 = getmillisecs();

    std::vector<idx_t> Iq(params->nprobe * n);
    std::vector<float> Dq(params->nprobe * n);

    const IndexIVF* index_ivf = dynamic_cast<const IndexIVF*>(index);
    FAISS_THROW_IF_NOT(index_ivf);

    SearchParameters* quantizer_params =
            (params) ? params->quantizer_params : nullptr;
    index_ivf->quantizer->search(
            n, x, params->nprobe, Dq.data(), Iq.data(), quantizer_params);

    if (nb_dis_ptr) {
        *nb_dis_ptr = count_ndis(index_ivf, n * params->nprobe, Iq.data());
    }

    double t2 = getmillisecs();

    index_ivf->search_preassigned(
            n, x, k, Iq.data(), Dq.data(), distances, labels, false, params);
    double t3 = getmillisecs();

    if (ms_per_stage) {
        ms_per_stage[0] = t1 - t0;
        ms_per_stage[1] = t2 - t1;
        ms_per_stage[2] = t3 - t2;
    }
}

void range_search_with_parameters(
        const Index* index,
        idx_t n,
        const float* x,
        float radius,
        RangeSearchResult* result,
        const IVFSearchParameters* params,
        size_t* nb_dis_ptr,
        double* ms_per_stage) {
    FAISS_THROW_IF_NOT(params);
    const float* prev_x = x;
    std::unique_ptr<const float[]> del;

    double t0 = getmillisecs();

    if (auto ip = dynamic_cast<const IndexPreTransform*>(index)) {
        x = ip->apply_chain(n, x);
        if (x != prev_x) {
            del.reset(x);
        }
        index = ip->index;
    }

    double t1 = getmillisecs();

    std::vector<idx_t> Iq(params->nprobe * n);
    std::vector<float> Dq(params->nprobe * n);

    const IndexIVF* index_ivf = dynamic_cast<const IndexIVF*>(index);
    FAISS_THROW_IF_NOT(index_ivf);

    index_ivf->quantizer->search(n, x, params->nprobe, Dq.data(), Iq.data());

    if (nb_dis_ptr) {
        *nb_dis_ptr = count_ndis(index_ivf, n * params->nprobe, Iq.data());
    }

    double t2 = getmillisecs();

    index_ivf->range_search_preassigned(
            n, x, radius, Iq.data(), Dq.data(), result, false, params);

    double t3 = getmillisecs();
    if (ms_per_stage) {
        ms_per_stage[0] = t1 - t0;
        ms_per_stage[1] = t2 - t1;
        ms_per_stage[2] = t3 - t2;
    }
}

IndexIVFResidualQuantizer* ivf_residual_from_quantizer(
        const ResidualQuantizer& rq,
        int nlevel) {
    FAISS_THROW_IF_NOT(nlevel > 0 && nlevel + 1 < rq.M);

    std::vector<size_t> nbits(nlevel);
    std::copy(rq.nbits.begin(), rq.nbits.begin() + nlevel, nbits.begin());
    std::unique_ptr<ResidualCoarseQuantizer> rcq(
            new ResidualCoarseQuantizer(rq.d, nbits));

    // set the coarse quantizer from the 2 first quantizers
    rcq->rq.initialize_from(rq);
    rcq->is_trained = true;
    rcq->ntotal = (idx_t)1 << rcq->rq.tot_bits;

    // settings for exhaustive search in RCQ
    rcq->centroid_norms.resize(rcq->ntotal);
    rcq->aq->compute_centroid_norms(rcq->centroid_norms.data());
    rcq->beam_factor = -1.0; // use exact search
    size_t nlist = rcq->ntotal;

    // build a IVFResidualQuantizer from that
    std::vector<size_t> nbits_refined;
    for (int i = nlevel; i < rq.M; i++) {
        nbits_refined.push_back(rq.nbits[i]);
    }
    std::unique_ptr<IndexIVFResidualQuantizer> index(
            new IndexIVFResidualQuantizer(
                    rcq.get(),
                    rq.d,
                    nlist,
                    nbits_refined,
                    faiss::METRIC_L2,
                    rq.search_type));
    index->own_fields = true;
    rcq.release();
    index->by_residual = true;
    index->rq.initialize_from(rq, nlevel);
    index->is_trained = true;

    return index.release();
}

void ivf_residual_add_from_flat_codes(
        IndexIVFResidualQuantizer* index,
        size_t nb,
        const uint8_t* raw_codes,
        int64_t code_size) {
    const ResidualCoarseQuantizer* rcq =
            dynamic_cast<const faiss::ResidualCoarseQuantizer*>(
                    index->quantizer);
    FAISS_THROW_IF_NOT_MSG(rcq, "the coarse quantizer must be a RCQ");
    if (code_size < 0) {
        code_size = index->code_size;
    }
    InvertedLists& invlists = *index->invlists;
    const ResidualQuantizer& rq = index->rq;

    // populate inverted lists
#pragma omp parallel if (nb > 10000)
    {
        std::vector<uint8_t> tmp_code(index->code_size);
        std::vector<float> tmp(rq.d);
        int nt = omp_get_num_threads();
        int rank = omp_get_thread_num();

#pragma omp for
        for (idx_t i = 0; i < nb; i++) {
            const uint8_t* code = &raw_codes[i * code_size];
            BitstringReader rd(code, code_size);
            idx_t list_no = rd.read(rcq->rq.tot_bits);

            if (list_no % nt ==
                rank) { // each thread takes care of 1/nt of the invlists
                // copy AQ indexes one by one
                BitstringWriter wr(tmp_code.data(), tmp_code.size());
                for (int j = 0; j < rq.M; j++) {
                    int nbit = rq.nbits[j];
                    wr.write(rd.read(nbit), nbit);
                }
                // we need to recompute the norm
                // decode first, does not use the norm component, so that's
                // ok
                index->rq.decode(tmp_code.data(), tmp.data(), 1);
                float norm = fvec_norm_L2sqr(tmp.data(), rq.d);
                wr.write(rq.encode_norm(norm), rq.norm_bits);

                // add code to the inverted list
                invlists.add_entry(list_no, i, tmp_code.data());
            }
        }
    }
    index->ntotal += nb;
}

int64_t DefaultShardingFunction::operator()(int64_t i, int64_t shard_count) {
    return i % shard_count;
}

void handle_ivf(
        faiss::IndexIVF* index,
        int64_t shard_count,
        const std::string& filename_template,
        ShardingFunction* sharding_function,
        bool generate_ids) {
    std::vector<faiss::IndexIVF*> sharded_indexes(shard_count);
    auto clone = static_cast<faiss::IndexIVF*>(faiss::clone_index(index));
    clone->quantizer->reset();
    for (int64_t i = 0; i < shard_count; i++) {
        sharded_indexes[i] =
                static_cast<faiss::IndexIVF*>(faiss::clone_index(clone));
        if (generate_ids) {
            // Assume the quantizer does not natively support add_with_ids.
            sharded_indexes[i]->quantizer =
                    new IndexIDMap2(sharded_indexes[i]->quantizer);
        }
    }

    // assign centroids to each sharded Index based on sharding_function, and
    // add them to the quantizer of each sharded index
    std::vector<std::vector<float>> sharded_centroids(shard_count);
    std::vector<std::vector<idx_t>> xids(shard_count);
    for (int64_t i = 0; i < index->quantizer->ntotal; i++) {
        int64_t shard_id = (*sharding_function)(i, shard_count);
        // Since the quantizer does not natively support add_with_ids, we simply
        // generate them.
        xids[shard_id].push_back(i);
        float* reconstructed = new float[index->quantizer->d];
        index->quantizer->reconstruct(i, reconstructed);
        sharded_centroids[shard_id].insert(
                sharded_centroids[shard_id].end(),
                &reconstructed[0],
                &reconstructed[index->quantizer->d]);
        delete[] reconstructed;
    }
    for (int64_t i = 0; i < shard_count; i++) {
        if (generate_ids) {
            sharded_indexes[i]->quantizer->add_with_ids(
                    sharded_centroids[i].size() / index->quantizer->d,
                    sharded_centroids[i].data(),
                    xids[i].data());
        } else {
            sharded_indexes[i]->quantizer->add(
                    sharded_centroids[i].size() / index->quantizer->d,
                    sharded_centroids[i].data());
        }
    }

    for (int64_t i = 0; i < shard_count; i++) {
        char fname[256];
        snprintf(fname, 256, filename_template.c_str(), i);
        faiss::write_index(sharded_indexes[i], fname);
    }

    for (int64_t i = 0; i < shard_count; i++) {
        delete sharded_indexes[i];
    }
}

void handle_binary_ivf(
        faiss::IndexBinaryIVF* index,
        int64_t shard_count,
        const std::string& filename_template,
        ShardingFunction* sharding_function,
        bool generate_ids) {
    std::vector<faiss::IndexBinaryIVF*> sharded_indexes(shard_count);

    auto clone = static_cast<faiss::IndexBinaryIVF*>(
            faiss::clone_binary_index(index));
    clone->quantizer->reset();

    for (int64_t i = 0; i < shard_count; i++) {
        sharded_indexes[i] = static_cast<faiss::IndexBinaryIVF*>(
                faiss::clone_binary_index(clone));
        if (generate_ids) {
            // Assume the quantizer does not natively support add_with_ids.
            sharded_indexes[i]->quantizer =
                    new IndexBinaryIDMap2(sharded_indexes[i]->quantizer);
        }
    }

    // assign centroids to each sharded Index based on sharding_function, and
    // add them to the quantizer of each sharded index
    int64_t reconstruction_size = index->quantizer->d / 8;
    std::vector<std::vector<uint8_t>> sharded_centroids(shard_count);
    std::vector<std::vector<idx_t>> xids(shard_count);
    for (int64_t i = 0; i < index->quantizer->ntotal; i++) {
        int64_t shard_id = (*sharding_function)(i, shard_count);
        // Since the quantizer does not natively support add_with_ids, we simply
        // generate them.
        xids[shard_id].push_back(i);
        uint8_t* reconstructed = new uint8_t[reconstruction_size];
        index->quantizer->reconstruct(i, reconstructed);
        sharded_centroids[shard_id].insert(
                sharded_centroids[shard_id].end(),
                &reconstructed[0],
                &reconstructed[reconstruction_size]);
        delete[] reconstructed;
    }
    for (int64_t i = 0; i < shard_count; i++) {
        if (generate_ids) {
            sharded_indexes[i]->quantizer->add_with_ids(
                    sharded_centroids[i].size() / reconstruction_size,
                    sharded_centroids[i].data(),
                    xids[i].data());
        } else {
            sharded_indexes[i]->quantizer->add(
                    sharded_centroids[i].size() / reconstruction_size,
                    sharded_centroids[i].data());
        }
    }

    for (int64_t i = 0; i < shard_count; i++) {
        char fname[256];
        snprintf(fname, 256, filename_template.c_str(), i);
        faiss::write_index_binary(sharded_indexes[i], fname);
    }

    for (int64_t i = 0; i < shard_count; i++) {
        delete sharded_indexes[i];
    }
}

template <typename IndexType>
void sharding_helper(
        IndexType* index,
        int64_t shard_count,
        const std::string& filename_template,
        ShardingFunction* sharding_function,
        bool generate_ids) {
    FAISS_THROW_IF_MSG(index->quantizer->ntotal == 0, "No centroids to shard.");
    FAISS_THROW_IF_MSG(
            filename_template.find("%d") == std::string::npos,
            "Invalid filename_template. Must contain format specifier for shard count.");

    DefaultShardingFunction default_sharding_function;
    if (sharding_function == nullptr) {
        sharding_function = &default_sharding_function;
    }

    if (typeid(IndexType) == typeid(faiss::IndexIVF)) {
        handle_ivf(
                dynamic_cast<faiss::IndexIVF*>(index),
                shard_count,
                filename_template,
                sharding_function,
                generate_ids);
    } else if (typeid(IndexType) == typeid(faiss::IndexBinaryIVF)) {
        handle_binary_ivf(
                dynamic_cast<faiss::IndexBinaryIVF*>(index),
                shard_count,
                filename_template,
                sharding_function,
                generate_ids);
    }
}

void shard_ivf_index_centroids(
        faiss::IndexIVF* index,
        int64_t shard_count,
        const std::string& filename_template,
        ShardingFunction* sharding_function,
        bool generate_ids) {
    sharding_helper(
            index,
            shard_count,
            filename_template,
            sharding_function,
            generate_ids);
}

void shard_binary_ivf_index_centroids(
        faiss::IndexBinaryIVF* index,
        int64_t shard_count,
        const std::string& filename_template,
        ShardingFunction* sharding_function,
        bool generate_ids) {
    sharding_helper(
            index,
            shard_count,
            filename_template,
            sharding_function,
            generate_ids);
}

} // namespace ivflib
} // namespace faiss
