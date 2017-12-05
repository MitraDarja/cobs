#include <isi/util/query.hpp>
#include <isi/util/error_handling.hpp>
#include <zconf.h>
#include <algorithm>

namespace isi::query {
    std::pair<int, uint8_t*> initialize_mmap(const std::experimental::filesystem::path& path, const stream_metadata& smd) {
        int fd = open(path.string().data(), O_RDONLY, 0);
        if(fd == -1) {
            exit_error_errno("could not open index file " + path.string());
        }

        void* mmap_ptr = mmap(NULL, smd.end_pos, PROT_READ, MAP_PRIVATE, fd, 0);
        if(mmap_ptr == MAP_FAILED) {
            exit_error_errno("mmap failed");
        }
        if(madvise(mmap_ptr, smd.end_pos, MADV_RANDOM)) {
            print_errno("madvise failed");
        }
        return {fd, smd.curr_pos + reinterpret_cast<uint8_t*>(mmap_ptr)};
    }

    void destroy_mmap(int fd, uint8_t* mmap_ptr, const stream_metadata& smd) {
        if (munmap(mmap_ptr - smd.curr_pos, smd.end_pos)) {
            print_errno("could not unmap index file");
        }
        if (close(fd)) {
            print_errno("could not close index file");
        }
    }

    const char* normalize_kmer(const char* query_8, char* kmer_raw_8, uint32_t kmer_size) {
        //todo use std::mismatch with std::reverse_iterator
        const char* query_8_reverse = query_8 + kmer_size;
        size_t counter = 0;
        while(*query_8 == *query_8_reverse && counter < kmer_size / 2) {
            query_8++;
            query_8_reverse--;
            counter++;
        }

        if(*query_8 <= *query_8_reverse) {
            return query_8 - counter;
        } else {
            std::reverse_copy(query_8 - counter, query_8_reverse + counter, kmer_raw_8);
            return kmer_raw_8;
        }
    }
}