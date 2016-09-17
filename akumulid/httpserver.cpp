#include "httpserver.h"
#include "utility.h"
#include <cstring>
#include <thread>

#include <boost/bind.hpp>

namespace Akumuli {
namespace Http {

//! Microhttpd callback functions
namespace MHD {
static ssize_t read_callback(void *data, uint64_t pos, char *buf, size_t max) {
    AKU_UNUSED(pos);
    ReadOperation* cur = (ReadOperation*)data;
    auto status = cur->get_error();
    if (status) {
        return MHD_CONTENT_READER_END_OF_STREAM;
    }
    size_t sz;
    bool is_done;
    std::tie(sz, is_done) = cur->read_some(buf, max);
    if (is_done) {
        return MHD_CONTENT_READER_END_OF_STREAM;
    } else {
        if (sz == 0u) {
            // Not at the end of the stream but data is not ready yet.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return sz;
}

static void free_callback(void *data) {
    ReadOperation* cur = (ReadOperation*)data;
    cur->close();
    delete cur;
}

static int accept_connection(void           *cls,
                             MHD_Connection *connection,
                             const char     *url,
                             const char     *method,
                             const char     *version,
                             const char     *upload_data,
                             size_t         *upload_data_size,
                             void          **con_cls)
{
    if (strcmp(method, "POST") == 0) {
        ReadOperationBuilder *queryproc = static_cast<ReadOperationBuilder*>(cls);
        ReadOperation* cursor = static_cast<ReadOperation*>(*con_cls);

        if (cursor == nullptr) {
            cursor = queryproc->create();
            *con_cls = cursor;
            return MHD_YES;
        }
        if (*upload_data_size) {
            cursor->append(upload_data, *upload_data_size);
            *upload_data_size = 0;
            return MHD_YES;
        }

        auto error_response = [&](const char* msg) {
            char buffer[0x200];
            int len = snprintf(buffer, 0x200, "-%s\r\n", msg);
            auto response = MHD_create_response_from_buffer(len, buffer, MHD_RESPMEM_MUST_COPY);
            int ret = MHD_queue_response(connection, MHD_HTTP_BAD_REQUEST, response);
            MHD_destroy_response(response);
            return ret;
        };

        // Should be called once
        try {
            cursor->start();
        } catch (const std::exception& err) {
            return error_response(err.what());
        }

        // Check for error
        auto err = cursor->get_error();
        if (err != AKU_SUCCESS) {
            const char* error_msg = aku_error_message(err);
            return error_response(error_msg);
        }

        auto response = MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, 64*1024, &read_callback, cursor, &free_callback);
        int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        return ret;
    } else {
        static const char* SIGIL = "";
        auto queryproc = static_cast<ReadOperationBuilder*>(cls);
        auto cursor = static_cast<const char*>(*con_cls);
        if (cursor == nullptr) {
            *con_cls = const_cast<char*>(SIGIL);
            return MHD_YES;
        }
        std::string path = url;
        if (path == "/stats") {
            std::string stats = queryproc->get_all_stats();
            auto response = MHD_create_response_from_buffer(stats.size(), const_cast<char*>(stats.data()), MHD_RESPMEM_MUST_COPY);
            int ret = MHD_add_response_header(response, "content-type", "application/json");
            if (ret == MHD_NO) {
                return ret;
            }
            ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
            MHD_destroy_response(response);
            return ret;
        }
    }
    return MHD_NO;
}
}

HttpServer::HttpServer(unsigned short port, std::shared_ptr<ReadOperationBuilder> qproc, AccessControlList const& acl)
    : acl_(acl)
    , proc_(qproc)
    , port_(port)
{
}

HttpServer::HttpServer(unsigned short port, std::shared_ptr<ReadOperationBuilder> qproc)
    : HttpServer(port, qproc, AccessControlList())
{
}

void HttpServer::start(SignalHandler* sig, int id) {
    daemon_ = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION,
                               port_,
                               NULL,
                               NULL,
                               &MHD::accept_connection,
                               proc_.get(),
                               MHD_OPTION_END);
    if (daemon_ == nullptr) {
        BOOST_THROW_EXCEPTION(std::runtime_error("can't start daemon"));
    }

    auto self = shared_from_this();
    sig->add_handler(boost::bind(&HttpServer::stop, std::move(self)), id);
}

void HttpServer::stop() {
    MHD_stop_daemon(daemon_);
}

struct HttpServerBuilder {

    HttpServerBuilder() {
        ServerFactory::instance().register_type("HTTP", *this);
    }

    std::shared_ptr<Server> operator () (std::shared_ptr<IngestionPipeline>,
                                         std::shared_ptr<ReadOperationBuilder> qproc,
                                         const ServerSettings& settings) {
        return std::make_shared<HttpServer>(settings.port, qproc);
    }
};

static HttpServerBuilder reg_type;

}  // namespace Http
}  // namespace Akumuli

