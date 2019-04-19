/*******************************************************************************
 * src/cobs.cpp
 *
 * Copyright (c) 2018 Florian Gauger
 * Copyright (c) 2018 Timo Bingmann
 *
 * All rights reserved. Published under the MIT License in the LICENSE file.
 ******************************************************************************/

#include <cobs/construction/classic_index.hpp>
#include <cobs/construction/compact_index.hpp>
#include <cobs/construction/ranfold_index.hpp>
#include <cobs/cortex_file.hpp>
#include <cobs/query/classic_index/mmap.hpp>
#include <cobs/query/classic_search.hpp>
#include <cobs/query/compact_index/mmap.hpp>
#include <cobs/settings.hpp>
#include <cobs/util/calc_signature_size.hpp>
#ifdef __linux__
    #include <cobs/query/compact_index/aio.hpp>
#endif

#include <tlx/cmdline_parser.hpp>
#include <tlx/string.hpp>

#include <cmath>
#include <map>
#include <random>
#include <unordered_map>
#include <unordered_set>

/******************************************************************************/

cobs::FileType StringToFileType(std::string& s) {
    tlx::to_lower(&s);
    if (s == "any" || s == "*")
        return cobs::FileType::Any;
    if (s == "text" || s == "txt")
        return cobs::FileType::Text;
    if (s == "cortex" || s == "ctx")
        return cobs::FileType::Cortex;
    if (s == "cobs" || s == "cobs_doc")
        return cobs::FileType::KMerBuffer;
    if (s == "fasta")
        return cobs::FileType::Fasta;
    if (s == "fastq")
        return cobs::FileType::Fastq;
    die("Unknown file type " << s);
}

/******************************************************************************/
// Document List and Dump

static void print_document_list(cobs::DocumentList& filelist,
                                size_t term_size) {
    size_t max_kmers = 0, total_kmers = 0;

    LOG1 << "--- document list (" << filelist.size() << " entries) ---";

    for (size_t i = 0; i < filelist.size(); ++i) {
        size_t num_terms = filelist[i].num_terms(term_size);
        LOG1 << "document[" << i << "] size " << filelist[i].size_
             << " " << term_size << "-mers " << num_terms
             << " : " << filelist[i].path_
             << " : " << filelist[i].name_;
        max_kmers = std::max(max_kmers, num_terms);
        total_kmers += num_terms;
    }
    LOG1 << "--- end of document list (" << filelist.size() << " entries) ---";

    double avg_kmers = static_cast<double>(total_kmers) / filelist.size();

    LOG1 << "documents: " << filelist.size();
    LOG1 << "maximum " << term_size << "-mers: " << max_kmers;
    LOG1 << "average " << term_size << "-mers: "
         << static_cast<size_t>(avg_kmers);
    LOG1 << "total " << term_size << "-mers: " << total_kmers;
}

int doc_list(int argc, char** argv) {
    tlx::CmdlineParser cp;

    std::string path;
    cp.add_param_string(
        "path", path,
        "path to documents to dump");

    std::string file_type = "any";
    cp.add_string(
        'T', "file_type", file_type,
        "filter documents by file type (any, text, cortex, fasta, etc)");

    unsigned term_size = 31;
    cp.add_unsigned(
        'k', "term_size", term_size,
        "term size (k-mer size), default: 31");

    if (!cp.sort().process(argc, argv))
        return -1;

    cobs::DocumentList filelist(path, StringToFileType(file_type));
    print_document_list(filelist, term_size);

    return 0;
}

int doc_dump(int argc, char** argv) {
    tlx::CmdlineParser cp;

    std::string path;
    cp.add_param_string(
        "path", path,
        "path to documents to dump");

    unsigned term_size = 31;
    cp.add_unsigned(
        'k', "term_size", term_size,
        "term size (k-mer size), default: 31");

    std::string file_type = "any";
    cp.add_string(
        'T', "file_type", file_type,
        "filter documents by file type (any, text, cortex, fasta, etc)");

    if (!cp.sort().process(argc, argv))
        return -1;

    cobs::DocumentList filelist(path, StringToFileType(file_type));

    std::cerr << "Found " << filelist.size() << " documents." << std::endl;

    for (size_t i = 0; i < filelist.size(); ++i) {
        std::cerr << "document[" << i << "] : " << filelist[i].path_
                  << " : " << filelist[i].name_ << std::endl;
        filelist[i].process_terms(
            term_size,
            [&](const cobs::string_view& t) {
                std::cout << t << '\n';
            });
        std::cout.flush();
        std::cerr << "document[" << i << "] : "
                  << filelist[i].num_terms(term_size) << " terms."
                  << std::endl;
    }

    return 0;
}

/******************************************************************************/
// "Classical" Index Construction

int classic_construct(int argc, char** argv) {
    tlx::CmdlineParser cp;

    cobs::ClassicIndexParameters index_params;
    index_params.num_hashes = 1;
    index_params.false_positive_rate = 0.3;

    std::string in_dir;
    cp.add_param_string(
        "in_dir", in_dir, "path to the input directory");

    std::string out_dir;
    cp.add_param_string(
        "out_dir", out_dir, "path to the output directory");

    std::string file_type = "any";
    cp.add_string(
        't', "file_type", file_type,
        "filter input documents by file type (any, text, cortex, fasta, etc)");

    cp.add_bytes(
        'm', "mem_bytes", index_params.mem_bytes,
        "memory in bytes to use, default: " +
        tlx::format_iec_units(index_params.mem_bytes));

    cp.add_unsigned(
        'h', "num_hashes", index_params.num_hashes,
        "number of hash functions, default: 1");

    cp.add_double(
        'f', "false_positive_rate", index_params.false_positive_rate,
        "false positive rate, default: 0.3");

    cp.add_unsigned(
        'k', "term_size", index_params.term_size,
        "term size (k-mer size), default: 31");

    bool canonicalize = false;
    cp.add_flag(
        'c', "canonicalize", canonicalize,
        "canonicalize DNA k-mers, default: false");

    bool clobber = false;
    cp.add_flag(
        'C', "clobber", clobber,
        "erase output directory if it exists");

    bool continue_ = false;
    cp.add_flag(
        "continue", continue_,
        "continue in existing output directory");

    cp.add_size_t(
        'T', "threads", index_params.num_threads,
        "number of threads to use, default: max cores");

    cp.add_flag(
        "keep-temporary", cobs::gopt_keep_temporary,
        "keep temporary files during construction");

    if (!cp.sort().process(argc, argv))
        return -1;

    cp.print_result(std::cerr);

    // bool to uint8_t
    index_params.canonicalize = canonicalize;

    // check output and maybe clobber
    if (cobs::fs::exists(out_dir)) {
        if (clobber) {
            cobs::fs::remove_all(out_dir);
        }
        else if (continue_) {
            // fall through
        }
        else {
            die("Output directory exists, will not overwrite without --clobber");
        }
    }

    cobs::DocumentList filelist(in_dir, StringToFileType(file_type));
    print_document_list(filelist, index_params.term_size);

    cobs::classic_construct(filelist, out_dir, index_params);

    return 0;
}

int classic_construct_random(int argc, char** argv) {
    tlx::CmdlineParser cp;

    std::string out_file;
    cp.add_param_string(
        "out_file", out_file, "path to the output file");

    size_t signature_size = 2 * 1024 * 1024;
    cp.add_bytes(
        's', "signature_size", signature_size,
        "number of bits of the signatures (vertical size), default: 2 Mi");

    unsigned num_documents = 10000;
    cp.add_unsigned(
        'n', "num_documents", num_documents,
        "number of random documents in index,"
        " default: " + std::to_string(num_documents));

    unsigned document_size = 1000000;
    cp.add_unsigned(
        'm', "document_size", document_size,
        "number of random 31-mers in document,"
        " default: " + std::to_string(document_size));

    unsigned num_hashes = 1;
    cp.add_unsigned(
        'h', "num_hashes", num_hashes,
        "number of hash functions, default: 1");

    size_t seed = std::random_device { } ();
    cp.add_size_t("seed", seed, "random seed");

    if (!cp.sort().process(argc, argv))
        return -1;

    cp.print_result(std::cerr);

    std::cerr << "Constructing random index"
              << ", num_documents = " << num_documents
              << ", signature_size = " << signature_size
              << ", result size = "
              << tlx::format_iec_units(num_documents / 8 * signature_size)
              << std::endl;

    cobs::classic_construct_random(
        out_file, signature_size, num_documents, document_size, num_hashes, seed);

    return 0;
}

/******************************************************************************/
// "Compact" Index Construction

int compact_construct(int argc, char** argv) {
    tlx::CmdlineParser cp;

    cobs::CompactIndexParameters index_params;
    index_params.num_hashes = 1;
    index_params.false_positive_rate = 0.3;

    std::string in_dir;
    cp.add_param_string(
        "in_dir", in_dir, "path to the input directory");

    std::string out_dir;
    cp.add_param_string(
        "out_dir", out_dir, "path to the output directory");

    cp.add_bytes(
        'm', "mem_bytes", index_params.mem_bytes,
        "memory in bytes to use, default: " +
        tlx::format_iec_units(index_params.mem_bytes));

    cp.add_unsigned(
        'h', "num_hashes", index_params.num_hashes,
        "number of hash functions, default: 1");

    cp.add_double(
        'f', "false_positive_rate", index_params.false_positive_rate,
        "false positive rate, default: 0.3");

    cp.add_size_t(
        'p', "page_size", index_params.page_size,
        "the page size of the compact the index, "
        "default: sqrt(#documents)");

    bool clobber = false;
    cp.add_flag(
        'C', "clobber", clobber,
        "erase output directory if it exists");

    bool continue_ = false;
    cp.add_flag(
        "continue", continue_,
        "continue in existing output directory");

    cp.add_unsigned(
        'k', "term_size", index_params.term_size,
        "term size (k-mer size), default: 31");

    bool canonicalize = false;
    cp.add_flag(
        'c', "canonicalize", canonicalize,
        "canonicalize DNA k-mers, default: false");

    cp.add_size_t(
        'T', "threads", index_params.num_threads,
        "number of threads to use, default: max cores");

    cp.add_flag(
        "keep-temporary", cobs::gopt_keep_temporary,
        "keep temporary files during construction");

    if (!cp.sort().process(argc, argv))
        return -1;

    cp.print_result(std::cerr);

    // bool to uint8_t
    index_params.canonicalize = canonicalize;

    if (cobs::fs::exists(out_dir)) {
        if (clobber) {
            cobs::fs::remove_all(out_dir);
        }
        else if (continue_) {
            // fall through
        }
        else {
            die("Output directory exists, will not overwrite without --clobber");
        }
    }

    cobs::compact_construct(in_dir, out_dir, index_params);

    return 0;
}

int compact_construct_combine(int argc, char** argv) {
    tlx::CmdlineParser cp;

    std::string in_dir;
    cp.add_param_string(
        "in_dir", in_dir, "path to the input directory");

    std::string out_file;
    cp.add_param_string(
        "out_file", out_file, "path to the output file");

    unsigned page_size = 8192;
    cp.add_unsigned(
        'p', "page_size", page_size,
        "the page size of the compact the index, "
        "default: 8192");

    if (!cp.sort().process(argc, argv))
        return -1;

    cp.print_result(std::cerr);

    cobs::compact_combine_into_compact(in_dir, out_file, page_size);

    return 0;
}

/******************************************************************************/

int query(int argc, char** argv) {
    tlx::CmdlineParser cp;

    std::string in_file;
    cp.add_param_string(
        "in_file", in_file, "path to the input file");

    std::string query;
    cp.add_param_string(
        "query", query, "the dna sequence to search for");

    unsigned num_results = 100;
    cp.add_unsigned(
        'h', "num_results", num_results,
        "number of results to return, default: 100");

    if (!cp.sort().process(argc, argv))
        return -1;

    std::vector<std::pair<uint16_t, std::string> > result;
    cobs::Timer timer;

    if (cobs::file_has_header<cobs::ClassicIndexHeader>(in_file)) {
        cobs::query::classic_index::mmap mmap(in_file);
        cobs::query::ClassicSearch s(mmap);
        s.search(query, result, num_results);
        timer = s.timer();
    }
    else if (cobs::file_has_header<cobs::CompactIndexHeader>(in_file)) {
        cobs::query::compact_index::mmap mmap(in_file);
        cobs::query::ClassicSearch s(mmap);
        s.search(query, result, num_results);
        timer = s.timer();
    }
    else {
        die("Could not open index path \"" << in_file << "\"");
    }

    for (const auto& res : result) {
        std::cout << res.second << " - " << res.first << "\n";
    }
    std::cout << timer << std::endl;

    return 0;
}

/******************************************************************************/
// "Ranfold" Index Construction

int ranfold_construct(int argc, char** argv) {
    tlx::CmdlineParser cp;

    std::string in_dir;
    cp.add_param_string(
        "in_dir", in_dir, "path to the input directory");

    std::string out_file;
    cp.add_param_string(
        "out_file", out_file, "path to the output file");

    if (!cp.sort().process(argc, argv))
        return -1;

    cp.print_result(std::cerr);

    cobs::ranfold_index::construct(in_dir, out_file);

    return 0;
}

int ranfold_construct_random(int argc, char** argv) {
    tlx::CmdlineParser cp;

    std::string out_file;
    cp.add_param_string(
        "out_file", out_file, "path to the output file");

    size_t term_space = 2 * 1024 * 1024;
    cp.add_bytes(
        't', "term_space", term_space,
        "size of term space for kmer signatures (vertical size), "
        "default: " + tlx::format_iec_units(term_space));

    unsigned num_term_hashes = 1;
    cp.add_unsigned(
        'T', "term_hashes", num_term_hashes,
        "number of hash functions per term, default: 1");

    size_t doc_space_bits = 16 * 1024;
    cp.add_bytes(
        'd', "doc_space_bits", doc_space_bits,
        "number of bits in the document space (horizontal size), "
        "default: " + tlx::format_iec_units(doc_space_bits));

    unsigned num_doc_hashes = 2;
    cp.add_unsigned(
        'D', "doc_hashes", num_doc_hashes,
        "number of hash functions per term, default: 2");

    unsigned num_documents = 10000;
    cp.add_unsigned(
        'n', "num_documents", num_documents,
        "number of random documents in index,"
        " default: " + std::to_string(num_documents));

    unsigned document_size = 1000000;
    cp.add_unsigned(
        'm', "document_size", document_size,
        "number of random 31-mers in document,"
        " default: " + std::to_string(document_size));

    size_t seed = std::random_device { } ();
    cp.add_size_t("seed", seed, "random seed");

    if (!cp.sort().process(argc, argv))
        return -1;

    cp.print_result(std::cerr);

    std::cerr << "Constructing ranfold index"
              << ", term_space = " << term_space
              << ", num_term_hashes = " << num_term_hashes
              << ", doc_space_bits = " << doc_space_bits
              << ", num_doc_hashes = " << num_doc_hashes
              << ", num_documents = " << num_documents
              << ", document_size = " << document_size
              << ", result size = "
              << tlx::format_iec_units(
        (term_space * (doc_space_bits + 7) / 8))
              << std::endl;

    cobs::ranfold_index::construct_random(
        out_file,
        term_space, num_term_hashes,
        doc_space_bits, num_doc_hashes,
        num_documents, document_size,
        seed);

    return 0;
}

/******************************************************************************/
// Miscellaneous Methods

int print_parameters(int argc, char** argv) {
    tlx::CmdlineParser cp;

    unsigned num_hashes = 1;
    cp.add_unsigned(
        'h', "num_hashes", num_hashes,
        "number of hash functions, default: 1");

    double false_positive_rate = 0.3;
    cp.add_double(
        'f', "false_positive_rate", false_positive_rate,
        "false positive rate, default: 0.3");

    uint64_t num_elements = 0;
    cp.add_bytes(
        'n', "num_elements", num_elements,
        "number of elements to be inserted into the index");

    if (!cp.sort().process(argc, argv))
        return -1;

    if (num_elements == 0) {
        double signature_size_ratio =
            cobs::calc_signature_size_ratio(num_hashes, false_positive_rate);
        std::cout << signature_size_ratio << '\n';
    }
    else {
        uint64_t signature_size =
            cobs::calc_signature_size(num_elements, num_hashes, false_positive_rate);
        std::cout << "signature_size = " << signature_size << '\n';
        std::cout << "signature_bytes = " << signature_size / 8
                  << " = " << tlx::format_iec_units(signature_size / 8)
                  << '\n';
    }

    return 0;
}

int print_basepair_map(int, char**) {
    char basepair_map[256];
    memset(basepair_map, 0, sizeof(basepair_map));
    basepair_map['A'] = 'T';
    basepair_map['C'] = 'G';
    basepair_map['G'] = 'C';
    basepair_map['T'] = 'A';
    for (size_t i = 0; i < sizeof(basepair_map); ++i) {
        std::cout << int(basepair_map[i]) << ',';
        if (i % 16 == 15) {
            std::cout << '\n';
        }
    }
    return 0;
}

int print_kmers(int argc, char** argv) {
    tlx::CmdlineParser cp;

    std::string query;
    cp.add_param_string(
        "query", query, "the dna sequence to search for");

    unsigned kmer_size = 31;
    cp.add_unsigned(
        'k', "kmer_size", kmer_size,
        "the size of one kmer, default: 31");

    if (!cp.sort().process(argc, argv))
        return -1;

    char kmer_buffer[kmer_size];
    for (size_t i = 0; i < query.size() - kmer_size; i++) {
        auto kmer = cobs::canonicalize_kmer(
            query.data() + i, kmer_buffer, kmer_size);
        std::cout << std::string(kmer, kmer_size) << '\n';
    }

    return 0;
}

/******************************************************************************/
// Benchmarking

template <bool FalsePositiveDist>
void benchmark_fpr_run(const cobs::fs::path& p,
                       const std::vector<std::string>& queries,
                       const std::vector<std::string>& warmup_queries) {

    cobs::query::classic_index::mmap sf(p);
    cobs::query::ClassicSearch s(sf);

    sync();
    std::ofstream ofs("/proc/sys/vm/drop_caches");
    ofs << "3" << std::endl;
    ofs.close();

    std::vector<std::pair<uint16_t, std::string> > result;
    for (size_t i = 0; i < warmup_queries.size(); i++) {
        s.search(warmup_queries[i], result);
    }
    s.timer().reset();

    std::map<uint32_t, uint64_t> counts;

    for (size_t i = 0; i < queries.size(); i++) {
        s.search(queries[i], result);

        if (FalsePositiveDist) {
            for (const auto& r : result) {
                counts[r.first]++;
            }
        }
    }

    std::string sse2 = "off";
    std::string openmp = "on";
    std::string aio = "on";

#if __SSE2__
    sse2 = "on";
#endif

#ifdef NO_OPENMP
    openmp = "off";
#endif

#ifdef NO_AIO
    aio = "off";
#endif

    cobs::Timer t = s.timer();
    std::cout << "RESULT"
              << " name=benchmark "
              << " index=" << p.string()
              << " kmer_queries=" << (queries[0].size() - 30)
              << " queries=" << queries.size()
              << " warmup=" << warmup_queries.size()
              << " results=" << result.size()
              << " sse2=" << sse2
              << " openmp=" << openmp
              << " aio=" << aio
              << " t_hashes=" << t.get("hashes")
              << " t_io=" << t.get("io")
              << " t_and=" << t.get("and rows")
              << " t_add=" << t.get("add rows")
              << " t_sort=" << t.get("sort results")
              << std::endl;

    for (const auto& c : counts) {
        std::cout << "RESULT"
                  << " name=benchmark_fpr"
                  << " fpr=" << c.first
                  << " dist=" << c.second
                  << std::endl;
    }
}

int benchmark_fpr(int argc, char** argv) {
    tlx::CmdlineParser cp;

    std::string in_file;
    cp.add_param_string(
        "in_file", in_file, "path to the input file");

    unsigned num_kmers = 1000;
    cp.add_unsigned(
        'k', "num_kmers", num_kmers,
        "number of kmers of each query, "
        "default: " + std::to_string(num_kmers));

    unsigned num_queries = 10000;
    cp.add_unsigned(
        'q', "queries", num_queries,
        "number of random queries to run, "
        "default: " + std::to_string(num_queries));

    unsigned num_warmup = 100;
    cp.add_unsigned(
        'w', "warmup", num_warmup,
        "number of random warmup queries to run, "
        "default: " + std::to_string(num_warmup));

    bool fpr_dist = false;
    cp.add_flag(
        'd', "dist", fpr_dist,
        "calculate false positive distribution");

    size_t seed = std::random_device { } ();
    cp.add_size_t("seed", seed, "random seed");

    if (!cp.sort().process(argc, argv))
        return -1;

    std::mt19937 rng(seed);

    std::vector<std::string> warmup_queries;
    std::vector<std::string> queries;
    for (size_t i = 0; i < num_warmup; i++) {
        warmup_queries.push_back(
            cobs::random_sequence_rng(num_kmers + 30, rng));
    }
    for (size_t i = 0; i < num_queries; i++) {
        queries.push_back(
            cobs::random_sequence_rng(num_kmers + 30, rng));
    }

    if (fpr_dist)
        benchmark_fpr_run</* FalsePositiveDist */ true>(
            in_file, queries, warmup_queries);
    else
        benchmark_fpr_run</* FalsePositiveDist */ false>(
            in_file, queries, warmup_queries);

    return 0;
}

/******************************************************************************/

int generate_queries(int argc, char** argv) {
    tlx::CmdlineParser cp;

    std::string path;
    cp.add_param_string(
        "path", path,
        "path to base documents");

    std::string file_type = "any";
    cp.add_string(
        't', "file_type", file_type,
        "filter documents by file type (any, text, cortex, fasta, etc)");

    cp.add_size_t(
        'T', "threads", cobs::gopt_threads,
        "number of threads to use, default: max cores");

    unsigned term_size = 31;
    cp.add_unsigned(
        'k', "term_size", term_size,
        "term size (k-mer size), default: 31");

    size_t num_positive = 0;
    cp.add_size_t(
        'p', "positive", num_positive,
        "pick this number of existing positive queries, default: 0");

    size_t num_negative = 0;
    cp.add_size_t(
        'n', "negative", num_negative,
        "construct this number of random non-existing negative queries, "
        "default: 0");

    bool true_negatives = false;
    cp.add_flag(
        'N', "true-negative", true_negatives,
        "check that negative queries actually are not in the documents (slow)");

    size_t fixed_size = 0;
    cp.add_size_t(
        's', "size", fixed_size,
        "extend positive terms with random data to this size");

    size_t seed = std::random_device { } ();
    cp.add_size_t(
        'S', "seed", seed,
        "random seed");

    std::string out_file;
    cp.add_string(
        'o', "out_file", out_file,
        "output file path");

    if (!cp.sort().process(argc, argv))
        return -1;

    cobs::DocumentList filelist(path, StringToFileType(file_type));

    std::vector<size_t> terms_prefixsum;
    terms_prefixsum.reserve(filelist.size());
    size_t total_terms = 0;
    for (size_t i = 0; i < filelist.size(); ++i) {
        terms_prefixsum.push_back(total_terms);
        total_terms += filelist[i].num_terms(term_size);
    }

    LOG1 << "Given " << filelist.size() << " documents containing "
         << total_terms << " " << term_size << "-gram terms";

    if (fixed_size < term_size)
        fixed_size = term_size;

    std::default_random_engine rng(seed);

    struct Query {
        // term string
        std::string term;
        // document index
        size_t doc_index;
        // term index inside document
        size_t term_index;
    };

    // select random term ids for positive queries
    std::vector<size_t> positive_indices;
    std::vector<Query> positives;
    {
        die_unless(total_terms >= num_positive);

        std::unordered_set<size_t> positive_set;
        while (positive_set.size() < num_positive) {
            positive_set.insert(rng() % total_terms);
        }

        positive_indices.reserve(num_positive);
        for (auto it = positive_set.begin(); it != positive_set.end(); ++it) {
            positive_indices.push_back(*it);
        }
        std::sort(positive_indices.begin(), positive_indices.end());

        positives.resize(positive_indices.size());
    }

    // generate random negative queries
    std::vector<std::string> negatives;
    std::mutex negatives_mutex;
    std::unordered_map<std::string, std::vector<size_t> > negative_terms;
    std::unordered_set<size_t> negative_hashes;
    for (size_t t = 0; t < 1.5 * num_negative; ++t) {
        std::string neg = cobs::random_sequence_rng(fixed_size, rng);
        negatives.push_back(neg);

        // insert and hash all terms in the query
        for (size_t i = 0; i + term_size <= neg.size(); ++i) {
            std::string term = neg.substr(i, term_size);
            negative_terms[term].push_back(t);
            negative_hashes.insert(tlx::hash_djb2(term));
        }
    }

    // run over all documents and fetch positive queries
    cobs::parallel_for(
        0, filelist.size(), cobs::gopt_threads,
        [&](size_t d) {
            size_t index = terms_prefixsum[d];
            // find starting iterator for positive_indices in this file
            size_t pos_index = std::lower_bound(
                positive_indices.begin(), positive_indices.end(), index)
                               - positive_indices.begin();
            size_t next_index =
                pos_index < positive_indices.size() ?
                positive_indices[pos_index] : size_t(-1);
            if (next_index == size_t(-1) && !true_negatives)
                return;

            filelist[d].process_terms(
                term_size,
                [&](const cobs::string_view& term) {
                    if (index == next_index) {
                        // store positive term
                        Query& q = positives[pos_index];
                        q.term = term.to_string();
                        q.doc_index = d;
                        q.term_index = index - terms_prefixsum[d];

                        // extend positive term to term_size by padding
                        if (q.term.size() < fixed_size) {
                            size_t padding = fixed_size - q.term.size();
                            size_t front_padding = rng() % padding;
                            size_t back_padding = padding - front_padding;

                            q.term =
                                cobs::random_sequence_rng(front_padding, rng) +
                                q.term +
                                cobs::random_sequence_rng(back_padding, rng);
                        }

                        ++pos_index;
                        next_index = pos_index < positive_indices.size() ?
                                     positive_indices[pos_index] : size_t(-1);
                    }
                    index++;

                    if (true_negatives) {
                        // lookup hash of term in read-only hash table first
                        std::string t = term.to_string();
                        if (negative_hashes.count(tlx::hash_djb2(t)) != 0) {
                            std::unique_lock<std::mutex> lock(negatives_mutex);
                            auto it = negative_terms.find(t);
                            if (it != negative_terms.end()) {
                                // clear negative query with term found
                                LOG1 << "remove false negative: " << t;
                                for (const size_t& i : it->second)
                                    negatives[i].clear();
                                negative_terms.erase(it);
                            }
                        }
                    }
                });
        });

    // check if there are enough negatives queries left
    if (negatives.size() < num_negative) {
        die("Not enough true negatives left, you were unlucky, try again.");
    }

    // collect everything and return them in random order
    std::vector<Query> queries = std::move(positives);
    queries.reserve(num_positive + num_negative);

    size_t ni = 0;
    for (auto it = negatives.begin(); ni < num_negative; ++it) {
        if (it->empty())
            continue;
        Query q;
        q.term = std::move(*it);
        q.doc_index = size_t(-1);
        queries.emplace_back(q);
        ++ni;
    }

    std::shuffle(queries.begin(), queries.end(), rng);

    auto write_output =
        [&](std::ostream& os) {
            for (const Query& q : queries) {
                if (q.doc_index != size_t(-1))
                    os << ">doc:" << q.doc_index << ":term:" << q.term_index
                       << ":" << filelist[q.doc_index].name_ << '\n';
                else
                    os << ">negative" << '\n';
                os << q.term << '\n';
            }
        };

    if (out_file.empty()) {
        write_output(std::cout);
    }
    else {
        std::ofstream os(out_file.c_str());
        write_output(os);
    }

    return 0;
}

/******************************************************************************/

struct SubTool {
    const char* name;
    int (* func)(int argc, char* argv[]);
    bool shortline;
    const char* description;
};

struct SubTool subtools[] = {
    {
        "doc_list", &doc_list, true,
        "read a list of documents and print the list"
    },
    {
        "doc_dump", &doc_dump, true,
        "read a list of documents and dump their contents"
    },
    {
        "classic_construct", &classic_construct, true,
        "constructs a classic index from the documents in <in_dir>"
    },
    {
        "classic_construct_random", &classic_construct_random, true,
        "constructs a classic index with random content"
    },
    {
        "compact_construct", &compact_construct, true,
        "creates the folders used for further construction"
    },
    {
        "compact_construct_combine", &compact_construct_combine, true,
        "combines the classic indices in <in_dir> to form a compact index"
    },
    {
        "ranfold_construct", &ranfold_construct, true,
        "constructs a ranfold index from documents"
    },
    {
        "ranfold_construct_random", &ranfold_construct_random, true,
        "constructs a ranfold index with random content"
    },
    {
        "query", &query, true,
        "query an index"
    },
    {
        "print_parameters", &print_parameters, true,
        "calculates index parameters"
    },
    {
        "print_kmers", &print_kmers, true,
        "print all canonical kmers from <query>"
    },
    {
        "print_basepair_map", &print_basepair_map, true,
        "print canonical basepair character mapping"
    },
    {
        "benchmark_fpr", &benchmark_fpr, true,
        "run benchmark and false positive measurement"
    },
    {
        "generate_queries", &generate_queries, true,
        "select queries randomly from documents"
    },
    { nullptr, nullptr, true, nullptr }
};

int main_usage(const char* arg0) {
    std::cout << "(Co)mpact (B)it-Sliced (S)ignature Index for Genome Search"
              << std::endl << std::endl;
    std::cout << "Usage: " << arg0 << " <subtool> ..." << std::endl << std::endl
              << "Available subtools: " << std::endl;

    int shortlen = 0;

    for (size_t i = 0; subtools[i].name; ++i)
    {
        if (!subtools[i].shortline) continue;
        shortlen = std::max(shortlen, static_cast<int>(strlen(subtools[i].name)));
    }

    for (size_t i = 0; subtools[i].name; ++i)
    {
        if (subtools[i].shortline) continue;
        std::cout << "  " << subtools[i].name << std::endl;
        tlx::CmdlineParser::output_wrap(
            std::cout, subtools[i].description, 80, 6, 6);
        std::cout << std::endl;
    }

    for (size_t i = 0; subtools[i].name; ++i)
    {
        if (!subtools[i].shortline) continue;
        std::cout << "  " << std::left << std::setw(shortlen + 2)
                  << subtools[i].name << subtools[i].description << std::endl;
    }
    std::cout << std::endl;

    return 0;
}

int main(int argc, char** argv) {
    char progsub[256];

    if (argc < 2)
        return main_usage(argv[0]);

    for (size_t i = 0; subtools[i].name; ++i)
    {
        if (strcmp(subtools[i].name, argv[1]) == 0)
        {
            // replace argv[1] with call string of subtool.
            snprintf(progsub, sizeof(progsub), "%s %s", argv[0], argv[1]);
            argv[1] = progsub;
            try {
                return subtools[i].func(argc - 1, argv + 1);
            }
            catch (std::exception& e) {
                std::cerr << "EXCEPTION: " << e.what() << std::endl;
                return -1;
            }
        }
    }

    std::cout << "Unknown subtool '" << argv[1] << "'" << std::endl;

    return main_usage(argv[0]);
}

/******************************************************************************/
