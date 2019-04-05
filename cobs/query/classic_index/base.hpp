/*******************************************************************************
 * cobs/query/classic_index/base.hpp
 *
 * Copyright (c) 2018 Florian Gauger
 *
 * All rights reserved. Published under the MIT License in the LICENSE file.
 ******************************************************************************/

#ifndef COBS_QUERY_CLASSIC_INDEX_BASE_HEADER
#define COBS_QUERY_CLASSIC_INDEX_BASE_HEADER

#include <cobs/file/classic_index_header.hpp>
#include <cobs/query/index_file.hpp>

namespace cobs::query::classic_index {

class base : public query::IndexFile
{
protected:
    explicit base(const fs::path& path);

    uint32_t term_size() const override { return header_.term_size(); }
    uint8_t canonicalize() const override { return header_.canonicalize(); }
    uint64_t num_hashes() const override { return header_.num_hashes(); }
    uint64_t row_size() const override { return header_.row_size(); }
    uint64_t page_size() const override { return 1; }
    uint64_t counts_size() const override;
    const std::vector<std::string>& file_names() const override {
        return header_.file_names();
    }

    ClassicIndexHeader header_;

public:
    virtual ~base() = default;
};

} // namespace cobs::query::classic_index

#endif // !COBS_QUERY_CLASSIC_INDEX_BASE_HEADER

/******************************************************************************/
