#include <iostream>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <queue>
#include <chrono>
#include <faiss/ProductQuantizer.h>
#include <faiss/index_io.h>

#include "IndexIVF_HNSW.h"
#include "hnswlib/hnswlib.h"

#include <map>
#include <set>
#include <unordered_set>

#include "Parser.h"

using namespace std;
using namespace hnswlib;
using namespace ivfhnsw;

enum class Dataset
{
    DEEP1B,
    SIFT1B
};

void demo_sift1b(int argc, char **argv)
{
//    save_groups_sift("/home/dbaranchuk/data/groups/sift1B_groups.bvecs",
//                     "/home/dbaranchuk/data/bigann/bigann_base.bvecs",
//                     "/home/dbaranchuk/sift1B_precomputed_idxs_993127.ivecs",
//                     993127, 128, vecsize);
//    exit(0);
    /** Parse Options **/
    Parser opt = Parser(argc, argv);

    cout << "Loading GT:\n";
    std::vector<idx_t>massQA(opt.qsize * opt.gtdim);
    std::ifstream gt_input(opt.path_gt, ios::binary);
    readXvec<idx_t>(gt_input, massQA.data(), opt.gtdim, opt.qsize);
    gt_input.close();

    cout << "Loading queries:\n";
    std::vector<float> massQ(opt.qsize * opt.vecdim);
    std::ifstream query_input(opt.path_q, ios::binary);
    readXvecFvec<uint8_t >(query_input, massQ.data(), opt.vecdim, opt.qsize);
    query_input.close();

    SpaceInterface<float> *l2space = new L2Space(opt.vecdim);

    /** Create Index **/
    //IndexIVF_HNSW_Grouping *index = new IndexIVF_HNSW_Grouping(vecdim, ncentroids, M_PQ, 8, nsubcentroids);
    IndexIVF_HNSW *index = new IndexIVF_HNSW(opt.vecdim, opt.ncentroids, opt.M_PQ, 8);
    index->buildCoarseQuantizer(l2space, opt.path_centroids,
                                opt.path_info, opt.path_edges,
                                opt.M, opt.efConstruction);

    /** Train PQ **/
    std::ifstream learn_input(opt.path_learn, ios::binary);
    int nt = 1000000;//262144;
    int sub_nt = 131072;//262144;//65536;
    std::vector<float> trainvecs(nt * opt.vecdim);
    readXvecFvec<uint8_t>(learn_input, trainvecs.data(), opt.vecdim, nt);
    learn_input.close();

    /** Set Random Subset of sub_nt trainvecs **/
    std::vector<float> trainvecs_rnd_subset(sub_nt * opt.vecdim);
    random_subset(trainvecs.data(), trainvecs_rnd_subset.data(), opt.vecdim, nt, sub_nt);

    /** Train PQ **/
    if (exists_test(opt.path_pq) && exists_test(opt.path_norm_pq)) {
        std::cout << "Loading Residual PQ codebook from " << opt.path_pq << std::endl;
        index->pq = faiss::read_ProductQuantizer(opt.path_pq);
        std::cout << index->pq->d << " " << index->pq->code_size << " " << index->pq->dsub
                  << " " << index->pq->ksub << " " << index->pq->centroids[0] << std::endl;

        std::cout << "Loading Norm PQ codebook from " << opt.path_norm_pq << std::endl;
        index->norm_pq = faiss::read_ProductQuantizer(opt.path_norm_pq);
        std::cout << index->norm_pq->d << " " << index->norm_pq->code_size << " " << index->norm_pq->dsub
                  << " " << index->norm_pq->ksub << " " << index->norm_pq->centroids[0] << std::endl;
    }
    else {
        std::cout << "Training PQ codebooks" << std::endl;
        index->train_pq(sub_nt, trainvecs_rnd_subset.data());

        std::cout << "Saving Residual PQ codebook to " << opt.path_pq << std::endl;
        faiss::write_ProductQuantizer(index->pq, opt.path_pq);

        std::cout << "Saving Norm PQ codebook to " << opt.path_norm_pq << std::endl;
        faiss::write_ProductQuantizer(index->norm_pq, opt.path_norm_pq);
    }

    if (exists_test(opt.path_index)){
        /** Load Index **/
        std::cout << "Loading index from " << opt.path_index << std::endl;
        index->read(opt.path_index);
    } else {
        /** Add elements **/
//      index->add<uint8_t>(opt.path_groups, opt.path_idxs);
        index->add<uint8_t>(opt.vecsize, opt.path_data, opt.path_precomputed_idxs);

        /** Save index, pq and norm_pq **/
        std::cout << "Saving index to " << opt.path_index << std::endl;
        std::cout << "       pq to " << opt.path_pq << std::endl;
        std::cout << "       norm pq to " << opt.path_norm_pq << std::endl;

        /** Computing Centroid Norms **/
        std::cout << "Computing centroid norms"<< std::endl;
        index->compute_centroid_norms();
        index->write(opt.path_index);
    }
    index->compute_s_c();

    /** Parse groundtruth **/
    std::vector<std::priority_queue< std::pair<float, labeltype >>> answers;
    std::cout << "Parsing gt\n";
    (std::vector<std::priority_queue< std::pair<float, labeltype >>>(opt.qsize)).swap(answers);
    for (int i = 0; i < opt.qsize; i++)
        answers[i].emplace(0.0f, massQA[opt.gtdim*i]);

    /** Set search parameters **/
    index->max_codes = opt.max_codes;
    index->nprobe = opt.nprobes;
    index->quantizer->ef_ = opt.efSearch;

    /** Search **/
    int correct = 0;
    float distances[opt.k];
    long labels[opt.k];

    StopW stopw = StopW();
    for (int i = 0; i < opt.qsize; i++) {
        for (int j = 0; j < opt.k; j++){
            distances[j] = 0;
            labels[j] = 0;
        }

        index->search(massQ.data() + i*opt.vecdim, opt.k, distances, labels);

        std::priority_queue<std::pair<float, labeltype >> gt(answers[i]);
        std::unordered_set<labeltype> g;

        while (gt.size()) {
            g.insert(gt.top().second);
            gt.pop();
        }

        for (int j = 0; j < opt.k; j++)
            if (g.count(labels[j]) != 0) {
                correct++;
                break;
            }
    }
    /**Represent results**/
    float time_us_per_query = stopw.getElapsedTimeMicro() / opt.qsize;
    std::cout << "Recall@" << opt.k << ": " << 1.0f * correct / opt.qsize << std::endl;
    std::cout << "Time per query: " << time_us_per_query << " us" << std::endl;
    //std::cout << "Average max_codes: " << index->average_max_codes / 10000 << std::endl;
    //std::cout << "Average reused q_s: " << (1.0 * index->counter_reused) / (index->counter_computed + index->counter_reused) << std::endl;
    //std::cout << "Average number of pruned points: " << (1.0 * index->filter_points) / 10000 << std::endl;

    //check_groupsizes(index, ncentroids);
    //std::cout << "Check precomputed idxs"<< std::endl;
    //check_precomputing(index, path_data, path_precomputed_idxs, vecdim, ncentroids, vecsize, gt_mistakes, gt_correct);

    delete index;
    delete l2space;
}

/**
 * Run IVF-HNSW on DEEP1B
 *
 * @param path_centroids
 * @param path_index
 * @param path_precomputed_idxs
 * @param path_pq
 * @param path_norm_pq
 * @param path_learn
 * @param path_data
 * @param path_q
 * @param path_gt
 * @param path_info
 * @param path_edges
 * @param path_groups
 * @param path_idxs
 * @param k
 * @param vecsize
 * @param qsize
 * @param vecdim
 * @param gt_dim
 * @param efConstruction
 * @param M
 * @param M_PQ
 * @param efSearch
 * @param nprobes
 * @param max_codes
 * @param ncentroids
 * @param nsubcentroids
 */

void demo_deep1b(int argc, char **argv)
{
    /** Parse Options **/
    Parser opt = Parser(argc, argv);

    cout << "Loading GT\n";
    std::vector<idx_t> massQA(opt.qsize * opt.gtdim);
    std::ifstream gt_input(opt.path_gt, ios::binary);
    readXvec<idx_t>(gt_input, massQA.data(), opt.gtdim, opt.qsize);
    gt_input.close();

    cout << "Loading queries\n";
    float massQ[opt.qsize * opt.vecdim];
    std::ifstream query_input(opt.path_q, ios::binary);
    readXvec<float>(query_input, massQ, opt.vecdim, opt.qsize);
    query_input.close();

    SpaceInterface<float> *l2space = new L2Space(opt.vecdim);

    /** Create Index **/
    //IndexIVF_HNSW_Grouping *index = new IndexIVF_HNSW_Grouping(opt.vecdim, opt.ncentroids, opt.M_PQ, 8, nsubcentroids);
    IndexIVF_HNSW *index = new IndexIVF_HNSW(opt.vecdim, opt.ncentroids, opt.M_PQ, 8);
    index->buildCoarseQuantizer(l2space, opt.path_centroids,
                                opt.path_info, opt.path_edges,
                                opt.M, opt.efConstruction);

    /** Train PQ **/
    std::ifstream learn_input(opt.path_learn, ios::binary);
    int nt = 1000000;//262144;
    int sub_nt = 131072;//262144;//65536;
    std::vector<float> trainvecs(nt * opt.vecdim);
    readXvec<float>(learn_input, trainvecs.data(), opt.vecdim, nt);
    learn_input.close();

    /** Set Random Subset of sub_nt trainvecs **/
    std::vector<float> trainvecs_rnd_subset(sub_nt * opt.vecdim);
    random_subset(trainvecs.data(), trainvecs_rnd_subset.data(), opt.vecdim, nt, sub_nt);

    /** Train PQ **/
    if (exists_test(opt.path_pq) && exists_test(opt.path_norm_pq)) {
        std::cout << "Loading Residual PQ codebook from " << opt.path_pq << std::endl;
        index->pq = faiss::read_ProductQuantizer(opt.path_pq);
        std::cout << index->pq->d << " " << index->pq->code_size << " " << index->pq->dsub
                  << " " << index->pq->ksub << " " << index->pq->centroids[0] << std::endl;

        std::cout << "Loading Norm PQ codebook from " << opt.path_norm_pq << std::endl;
        index->norm_pq = faiss::read_ProductQuantizer(opt.path_norm_pq);
        std::cout << index->norm_pq->d << " " << index->norm_pq->code_size << " " << index->norm_pq->dsub
                  << " " << index->norm_pq->ksub << " " << index->norm_pq->centroids[0] << std::endl;
    }
    else {
        std::cout << "Training PQ codebooks" << std::endl;
        index->train_pq(sub_nt, trainvecs_rnd_subset.data());

        std::cout << "Saving Residual PQ codebook to " << opt.path_pq << std::endl;
        faiss::write_ProductQuantizer(index->pq, opt.path_pq);

        std::cout << "Saving Norm PQ codebook to " << opt.path_norm_pq << std::endl;
        faiss::write_ProductQuantizer(index->norm_pq, opt.path_norm_pq);
    }

    if (exists_test(opt.path_index)){
        /** Load Index **/
        std::cout << "Loading index from " << opt.path_index << std::endl;
        index->read(opt.path_index);
    } else {
        /** Add elements **/

//      index->add<float>(opt.path_groups, opt.path_idxs);
        index->add<float>(opt.vecsize, opt.path_data, opt.path_precomputed_idxs);

        /** Save index, pq and norm_pq **/
        std::cout << "Saving index to " << opt.path_index << std::endl;
        std::cout << "       pq to " << opt.path_pq << std::endl;
        std::cout << "       norm pq to " << opt.path_norm_pq << std::endl;

        /** Computing Centroid Norms **/
        std::cout << "Computing centroid norms"<< std::endl;
        index->compute_centroid_norms();
        index->write(opt.path_index);
    }
    index->compute_s_c();

    /** Parse groundtruth **/
    vector<std::priority_queue< std::pair<float, labeltype >>> answers;
    std::cout << "Parsing gt\n";
    (vector<std::priority_queue< std::pair<float, labeltype >>>(opt.qsize)).swap(answers);
    for (int i = 0; i < opt.qsize; i++)
        answers[i].emplace(0.0f, massQA[opt.gtdim*i]);

    /** Set search parameters **/
    index->max_codes = opt.max_codes;
    index->nprobe = opt.nprobes;
    index->quantizer->ef_ = opt.efSearch;

    /** Search **/
    int correct = 0;
    float distances[opt.k];
    long labels[opt.k];

    StopW stopw = StopW();
    for (int i = 0; i < opt.qsize; i++) {
        for (int j = 0; j < opt.k; j++){
            distances[j] = 0;
            labels[j] = 0;
        }

        index->search(massQ + i*opt.vecdim, opt.k, distances, labels);

        std::priority_queue<std::pair<float, labeltype >> gt(answers[i]);
        unordered_set<labeltype> g;

        while (gt.size()) {
            g.insert(gt.top().second);
            gt.pop();
        }

        for (int j = 0; j < opt.k; j++)
            if (g.count(labels[j]) != 0) {
                correct++;
                break;
            }
    }
    /**Represent results**/
    float time_us_per_query = stopw.getElapsedTimeMicro() / opt.qsize;
    std::cout << "Recall@" << opt.k << ": " << 1.0f * correct / opt.qsize << std::endl;
    std::cout << "Time per query: " << time_us_per_query << " us" << std::endl;
    //std::cout << "Average max_codes: " << index->average_max_codes / 10000 << std::endl;
    //std::cout << "Average reused q_s: " << (1.0 * index->counter_reused) / (index->counter_computed + index->counter_reused) << std::endl;
    //std::cout << "Average number of pruned points: " << (1.0 * index->filter_points) / 10000 << std::endl;

    //check_groupsizes(index, ncentroids);
    //std::cout << "Check precomputed idxs"<< std::endl;
    //check_precomputing(index, path_data, path_precomputed_idxs, vecdim, ncentroids, vecsize, gt_mistakes, gt_correct);

    delete index;
    delete l2space;
}




































//void compute_average_distance(const char *path_data, const char *path_centroids, const char *path_precomputed_idxs,
//                              const int ncentroids, const int vecdim, const int vecsize)
//{
//    std::ifstream centroids_input(path_centroids, ios::binary);
//    std::vector<float> centroids(ncentroids*vecdim);
//    readXvec<float>(centroids_input, centroids.data(), vecdim, ncentroids);
//    centroids_input.close();
//
//    const int batch_size = 1000000;
//    std::ifstream base_input(path_data, ios::binary);
//    std::ifstream idx_input(path_precomputed_idxs, ios::binary);
//    std::vector<float> batch(batch_size * vecdim);
//    std::vector<idx_t> idx_batch(batch_size);
//
//    double average_dist = 0.0;
//    for (int b = 0; b < (vecsize / batch_size); b++) {
//        readXvec<idx_t>(idx_input, idx_batch.data(), batch_size, 1);
//        readXvec<float>(base_input, batch.data(), vecdim, batch_size);
//
//        for (size_t i = 0; i < batch_size; i++) {
//            const float *centroid = centroids.data() + idx_batch[i] * vecdim;
//            average_dist += faiss::fvec_L2sqr(batch.data() + i*vecdim, centroid, vecdim);
//        }
//
//        if (b % 10 == 0) printf("%.1f %c \n", (100. * b) / (vecsize / batch_size), '%');
//    }
//    idx_input.close();
//    base_input.close();
//
//    std::cout << "Average: " << average_dist / 1000000000 << std::endl;
//}
//
//
//void compute_average_distance_sift(const char *path_data, const char *path_centroids, const char *path_precomputed_idxs,
//                              const int ncentroids, const int vecdim, const int vecsize)
//{
//    std::ifstream centroids_input(path_centroids, ios::binary);
//    std::vector<float> centroids(ncentroids*vecdim);
//    readXvec<float>(centroids_input, centroids.data(), vecdim, ncentroids);
//    centroids_input.close();
//
//    const int batch_size = 1000000;
//    std::ifstream base_input(path_data, ios::binary);
//    std::ifstream idx_input(path_precomputed_idxs, ios::binary);
//    std::vector<float > batch(batch_size * vecdim);
//    std::vector<idx_t> idx_batch(batch_size);
//
//    double average_dist = 0.0;
//    for (int b = 0; b < (vecsize / batch_size); b++) {
//        readXvec<idx_t>(idx_input, idx_batch.data(), batch_size, 1);
//        readXvecFvec<uint8_t>(base_input, batch.data(), vecdim, batch_size);
//
//        for (size_t i = 0; i < batch_size; i++) {
//            const float *centroid = centroids.data() + idx_batch[i] * vecdim;
//            average_dist += faiss::fvec_L2sqr(batch.data() + i*vecdim, centroid, vecdim);
//        }
//
//        if (b % 10 == 0) printf("%.1f %c \n", (100. * b) / (vecsize / batch_size), '%');
//    }
//    idx_input.close();
//    base_input.close();
//
//    std::cout << "Average: " << average_dist / vecsize << std::endl;
//}
//static void check_precomputing(IndexIVF_HNSW *index, const char *path_data, const char *path_precomputed_idxs,
//                               size_t vecdim, size_t ncentroids, size_t vecsize,
//                               std::set<idx_t> gt_mistakes, std::set<idx_t> gt_correct)
//{
//    size_t batch_size = 1000000;
//    std::ifstream base_input(path_data, ios::binary);
//    std::ifstream idx_input(path_precomputed_idxs, ios::binary);
//    std::vector<float> batch(batch_size * vecdim);
//    std::vector<idx_t> idx_batch(batch_size);
//
////    int counter = 0;
//    std::vector<float> mistake_dst;
//    std::vector<float> correct_dst;
//    for (int b = 0; b < (vecsize / batch_size); b++) {
//        readXvec<idx_t>(idx_input, idx_batch.data(), batch_size, 1);
//        readXvec<float>(base_input, batch.data(), vecdim, batch_size);
//
//        printf("%.1f %c \n", (100.*b)/(vecsize/batch_size), '%');
//
//        for (int i = 0; i < batch_size; i++) {
//            int elem = batch_size*b + i;
//            //float min_dist = 1000000;
//            //int min_centroid = 100000000;
//
//            if (gt_mistakes.count(elem) == 0 &&
//                gt_correct.count(elem) == 0)
//                continue;
//
//            float *data = batch.data() + i*vecdim;
//            for (int j = 0; j < ncentroids; j++) {
//                float *centroid = (float *) index->quantizer->getDataByInternalId(j);
//                float dist = faiss::fvec_L2sqr(data, centroid, vecdim);
//                //if (dist < min_dist){
//                //    min_dist = dist;
//                //    min_centroid = j;
//                //}
//                if (gt_mistakes.count(elem) != 0)
//                    mistake_dst.push_back(dist);
//                if (gt_correct.count(elem) != 0)
//                    correct_dst.push_back(dist);
//            }
////            if (min_centroid != idx_batch[i]){
////                std::cout << "Element: " << elem << " True centroid: " << min_centroid << " Precomputed centroid:" << idx_batch[i] << std::endl;
////                counter++;
////            }
//        }
//    }
//
//    std::cout << "Correct distance distribution\n";
//    for (int i = 0; i < correct_dst.size(); i++)
//        std::cout << correct_dst[i] << std::endl;
//
//    std::cout << std::endl << std::endl << std::endl;
//    std::cout << "Mistake distance distribution\n";
//    for (int i = 0; i < mistake_dst.size(); i++)
//        std::cout << mistake_dst[i] << std::endl;
//
//    idx_input.close();
//    base_input.close();
//}
//









//void save_groups(IndexIVF_HNSW *index, const char *path_groups, const char *path_data,
//                 const char *path_precomputed_idxs, const int vecdim, const int vecsize)
//{
//    const int ncentroids = 999973;
//    std::vector<std::vector<float>> data(ncentroids);
//    std::vector<std::vector<idx_t>> idxs(ncentroids);
//
//    const int batch_size = 1000000;
//    std::ifstream base_input(path_data, ios::binary);
//    std::ifstream idx_input(path_precomputed_idxs, ios::binary);
//    std::vector<float> batch(batch_size * vecdim);
//    std::vector<idx_t> idx_batch(batch_size);
//
//    for (int b = 0; b < (vecsize / batch_size); b++) {
//        readXvec<idx_t>(idx_input, idx_batch.data(), batch_size, 1);
//        readXvec<float>(base_input, batch.data(), vecdim, batch_size);
//
//        for (size_t i = 0; i < batch_size; i++) {
//            //if (idx_batch[i] < 900000)
//            //    continue;
//
//            idx_t cur_idx = idx_batch[i];
//            //for (int d = 0; d < vecdim; d++)
//            //    data[cur_idx].push_back(batch[i * vecdim + d]);
//            idxs[cur_idx].push_back(b*batch_size + i);
//        }
//
//        if (b % 10 == 0) printf("%.1f %c \n", (100. * b) / (vecsize / batch_size), '%');
//    }
//    idx_input.close();
//    base_input.close();
//
//    //FILE *fout = fopen(path_groups, "wb");
//    const char *path_idxs = "/home/dbaranchuk/data/groups/sift1B_idxs9993127.ivecs";
//    FILE *fout = fopen(path_idxs, "wb");
//
////    size_t counter = 0;
//    for (int i = 0; i < ncentroids; i++) {
//        int groupsize = data[i].size() / vecdim;
////        counter += idxs[i].size();
//
//        if (groupsize != index->ids[i].size()){
//            std::cout << "Wrong groupsize: " << groupsize << " vs "
//                      << index->ids[i].size() <<std::endl;
//            exit(1);
//        }
//
//        fwrite(&groupsize, sizeof(int), 1, fout);
//        fwrite(idxs[i].data(), sizeof(idx_t), idxs[i].size(), fout);
//        //fwrite(data[i].data(), sizeof(float), data[i].size(), fout);
//    }
////    if (counter != 9993127){
////        std::cout << "Wrong poitns num\n";
////        exit(1);
////    }
//}




//void save_groups_sift(const char *path_groups, const char *path_data, const char *path_precomputed_idxs,
//                      const int ncentroids, const int vecdim, const int vecsize)
//{
//    //std::vector<std::vector<uint8_t >> data(ncentroids);
//    std::vector<std::vector<idx_t>> idxs(ncentroids);
//
//    const int batch_size = 1000000;
//    std::ifstream base_input(path_data, ios::binary);
//    std::ifstream idx_input(path_precomputed_idxs, ios::binary);
//    std::vector<uint8_t > batch(batch_size * vecdim);
//    std::vector<idx_t> idx_batch(batch_size);
//
//    for (int b = 0; b < (vecsize / batch_size); b++) {
//        readXvec<idx_t>(idx_input, idx_batch.data(), batch_size, 1);
//    //    readXvec<uint8_t >(base_input, batch.data(), vecdim, batch_size);
//
//        for (size_t i = 0; i < batch_size; i++) {
//            idx_t cur_idx = idx_batch[i];
//      //      for (int d = 0; d < vecdim; d++)
//      //          data[cur_idx].push_back(batch[i * vecdim + d]);
//            idxs[cur_idx].push_back(b*batch_size + i);
//        }
//
//        if (b % 10 == 0) printf("%.1f %c \n", (100. * b) / (vecsize / batch_size), '%');
//    }
//    idx_input.close();
//    base_input.close();
//
//    //FILE *fout = fopen(path_groups, "wb");
//    const char *path_idxs = "/home/dbaranchuk/data/groups/sift1B_idxs.ivecs";
//    FILE *fout = fopen(path_idxs, "wb");
//
//    size_t counter = 0;
//    for (int i = 0; i < ncentroids; i++) {
//        int groupsize = idxs[i].size();//data[i].size() / vecdim;
//        counter += groupsize;
//
//        fwrite(&groupsize, sizeof(int), 1, fout);
//        fwrite(idxs[i].data(), sizeof(idx_t), idxs[i].size(), fout);
//        //fwrite(data[i].data(), sizeof(uint8_t), data[i].size(), fout);
//    }
//    if (counter != vecsize){
//        std::cout << "Wrong poitns num\n";
//        exit(1);
//    }
//}

























//void check_groups(const char *path_data, const char *path_precomputed_idxs,
//                  const char *path_groups, const char *path_groups_idxs)
//{
//
//    const int vecsize = 1000000000;
//    const int d = 128;
//    /** Read Group **/
//    std::ifstream input_groups(path_groups, ios::binary);
//    std::ifstream input_groups_idxs(path_groups_idxs, ios::binary);
//
//    int groupsize, check_groupsize;
//    input_groups.read((char *) &groupsize, sizeof(int));
//    input_groups_idxs.read((char *) &check_groupsize, sizeof(int));
//    if (groupsize != check_groupsize){
//        std::cout << "Wrong groupsizes: " << groupsize << " " << check_groupsize << std::endl;
//        exit(1);
//    }
//
//    std::vector<uint8_t> group_b(groupsize*d);
//    std::vector<float> group(groupsize*d);
//    std::vector<idx_t> group_idxs(groupsize);
//
//    //input_groups.read((char *) group.data(), groupsize * d * sizeof(float));
//    input_groups.read((char *) group_b.data(), groupsize * d * sizeof(uint8_t));
//    for (int i = 0; i < groupsize*d; i++)
//        group[i] = (1.0)*group_b[i];
//
//    input_groups_idxs.read((char *) group_idxs.data(), groupsize * sizeof(idx_t));
//
//    input_groups.close();
//    input_groups_idxs.close();
//
//    /** Make set of idxs **/
//    std::unordered_set<idx_t > idx_set;
//    for (int i = 0; i < groupsize; i++)
//        idx_set.insert(group_idxs[i]);
//
//    /** Loop **/
//    const int batch_size = 1000000;
//    std::ifstream base_input(path_data, ios::binary);
//    std::ifstream idx_input(path_precomputed_idxs, ios::binary);
//    std::vector<float> batch(batch_size * d);
//    std::vector<idx_t> idx_batch(batch_size);
//
//    for (int b = 0; b < (vecsize / batch_size); b++) {
//        readXvec<idx_t>(idx_input, idx_batch.data(), batch_size, 1);
//        //readXvec<float>(base_input, batch.data(), d, batch_size);
//        readXvecFvec<uint8_t>(base_input, batch.data(), d, batch_size);
//
//        for (size_t i = 0; i < batch_size; i++) {
//            if (idx_set.count(b*batch_size + i) == 0)
//                continue;
//
//            const float *x = batch.data() + i*d;
//            for (int j = 0; j < groupsize; j++){
//                if (group_idxs[j] != b*batch_size + i)
//                    continue;
//
//                const float *y = group.data() + j * d;
//
//                std::cout << faiss::fvec_L2sqr(x, y, d) << std::endl;
//                break;
//            }
//        }
//
//        if (b % 10 == 0) printf("%.1f %c \n", (100. * b) / (vecsize / batch_size), '%');
//    }
//    idx_input.close();
//    base_input.close();
//}












//void check_groupsizes(IndexIVF_HNSW *index, int ncentroids)
//{
//    std::vector < size_t > groupsizes(ncentroids);
//
//    int sparse_counter = 0;
//    int big_counter = 0;
//    int small_counter = 0;
//    int other_counter = 0;
//    int giant_counter = 0;
//    for (int i = 0; i < ncentroids; i++){
//        int groupsize = index->norm_codes[i].size();
//        if (groupsize < 100)
//            sparse_counter++;
//        else if (groupsize > 100 && groupsize < 500)
//            small_counter++;
//        else if (groupsize > 1500 && groupsize < 3000)
//            big_counter++;
//        else if (groupsize > 3000)
//            giant_counter++;
//        else
//            other_counter++;
//    }
//
//    std::cout << "Number of clusters with size < 100: " << sparse_counter << std::endl;
//    std::cout << "Number of clusters with size > 100 && < 500 : " << small_counter << std::endl;
//
//    std::cout << "Number of clusters with size > 1500 && < 3000: " << big_counter << std::endl;
//    std::cout << "Number of clusters with size > 3000: " << giant_counter << std::endl;
//
//    std::cout << "Number of clusters with size > 500 && < 1500: " << other_counter << std::endl;
//}