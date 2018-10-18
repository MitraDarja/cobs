#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <isi/query/compact_index/mmap.hpp>


int main(int argc, char **argv) {
    std::array<uint32_t, 6> indices = {4096,  8192, 12288, 16384, 28672, 57344};

    std::string sample_name = "ERR498185";
    std::vector<std::string> queries;
    for (size_t i = 0; i < 1; i++) {
        queries.push_back(isi::random_sequence(1030, (size_t) time(nullptr)));
    }
    std::vector<std::pair<uint16_t, std::string>> result;

    for (uint32_t in: indices) {
        std::string index = "/well/iqbal/people/florian/indices/ena_" + std::to_string(in) +".com_idx.isi";

        std::map<size_t, size_t> counts;
        isi::query::compact_index::mmap s(index);
        for (size_t i = 0; i < queries.size(); i++) {
            s.search(queries[i], 31, result);
            for (const auto& r: result) {
                counts[r.first]++;
            }
        }
        for (auto a: counts) {
            std::cout << a.first << "," << a.second << "," << in << "\n";
        }
    }
}
