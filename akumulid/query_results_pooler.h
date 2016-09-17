#pragma once
#include "httpserver.h"
#include "ingestion_pipeline.h"
#include "server.h"
#include <memory>

namespace Akumuli {

//! Output formatter interface
struct OutputFormatter {
    virtual char* format(char* begin, char* end, const aku_Sample& sample) = 0;
};


struct QueryResultsPooler : ReadOperation {

    std::string                      query_text_;
    std::shared_ptr<DbConnection>    connection_;
    std::shared_ptr<DbCursor>        cursor_;
    std::unique_ptr<OutputFormatter> formatter_;

    std::vector<char>   rdbuf_;      //! Read buffer
    int                 rdbuf_pos_;  //! Read position in buffer
    int                 rdbuf_top_;  //! Last initialized item _index_ in `rdbuf_`
    static const size_t DEFAULT_RDBUF_SIZE_ = 1000u;
    static const size_t DEFAULT_ITEM_SIZE_  = sizeof(aku_Sample);

    QueryResultsPooler(std::shared_ptr<DbConnection> con, int readbufsize);

    void throw_if_started() const;

    void throw_if_not_started() const;

    virtual void start();

    virtual void append(const char* data, size_t data_size);

    virtual aku_Status get_error();

    virtual std::tuple<size_t, bool> read_some(char* buf, size_t buf_size);

    virtual void close();
};

struct QueryProcessor : ReadOperationBuilder {
    std::shared_ptr<DbConnection> con_;
    int                           rdbufsize_;

    QueryProcessor(std::shared_ptr<DbConnection> con, int rdbuf);

    virtual ReadOperation* create();

    virtual std::string get_all_stats();
};

}  // namespace
