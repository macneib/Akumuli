#include <iostream>
#include <memory>
#include <thread>

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE Main
#include <boost/test/unit_test.hpp>
#include <boost/algorithm/string.hpp>

#include "tcp_server.h"
#include "logger.h"

using namespace Akumuli;


static Logger logger_ = Logger("tcp-server-test", 10);


struct DbMock : DbConnection {
    typedef std::tuple<aku_ParamId, aku_Timestamp, double> ValueT;
    std::vector<ValueT> results;

    void close() {
    }

    aku_Status write(const aku_Sample &sample) {
        logger_.trace() << "write_double(" << sample.paramid << ", " << sample.timestamp << ", " << sample.payload.float64 << ")";
        results.push_back(std::make_tuple(sample.paramid, sample.timestamp, sample.payload.float64));
        return AKU_SUCCESS;
    }
    std::shared_ptr<DbCursor> search(std::string query) {
        throw "not implemented";
    }
    int param_id_to_series(aku_ParamId id, char *buffer, size_t buffer_size) {
        throw "not implemented";
    }
    aku_Status series_to_param_id(const char *name, size_t size, aku_Sample *sample) {
        throw "not implemented";
    }
    std::string get_all_stats() { throw "not impelemnted"; }
};


template<aku_Status ERR>
struct DbErrMock : DbConnection {
    aku_Status err = ERR;

    void close() {
    }

    aku_Status write(const aku_Sample &sample) {
        return err;
    }
    std::shared_ptr<DbCursor> search(std::string query) {
        throw "not implemented";
    }
    int param_id_to_series(aku_ParamId id, char *buffer, size_t buffer_size) {
        throw "not implemented";
    }
    aku_Status series_to_param_id(const char *name, size_t size, aku_Sample *sample) {
        throw "not implemented";
    }
    std::string get_all_stats() { throw "not impelemented"; }
};

const int PORT = 14096;

template<class Mock>
struct TCPServerTestSuite {
    std::shared_ptr<Mock>               dbcon;
    std::shared_ptr<IngestionPipeline>  pline;
    IOServiceT                          io;
    std::shared_ptr<TcpAcceptor>        serv;

    TCPServerTestSuite() {
        // Create mock pipeline
        dbcon = std::make_shared<Mock>();
        pline = std::make_shared<IngestionPipeline>(dbcon, AKU_LINEAR_BACKOFF);
        pline->start();

        // Run server
        std::vector<IOServiceT*> iovec = { &io };
        serv = std::make_shared<TcpAcceptor>(iovec, PORT, pline);

        // Start reading but don't start iorun thread
        serv->_start();
    }

    ~TCPServerTestSuite() {
        logger_.info() << "Clean up suite resources";
        if (serv) {
            serv->_stop();
        }
    }

    template<class Fn>
    void run(Fn const& fn) {
        // Connect to server
        SocketT socket(io);
        auto loopback = boost::asio::ip::address_v4::loopback();
        boost::asio::ip::tcp::endpoint peer(loopback, PORT);
        socket.connect(peer);
        serv->_run_one();  // run handle_accept one time

        // Run tests
        fn(socket);
    }
};


BOOST_AUTO_TEST_CASE(Test_tcp_server_loopback_1) {

    TCPServerTestSuite<DbMock> suite;

    suite.run([&](SocketT& socket) {
        boost::asio::streambuf stream;
        std::ostream os(&stream);
        os << ":1\r\n" << ":2\r\n" << "+3.14\r\n";

        boost::asio::write(socket, stream);

        // TCPSession.handle_read
        suite.io.run_one();
        suite.pline->stop();

        // Check
        if (suite.dbcon->results.size() != 1) {
            logger_.error() << "Error detected";
            BOOST_REQUIRE_EQUAL(suite.dbcon->results.size(), 1);
        }
        aku_ParamId id;
        aku_Timestamp ts;
        double value;
        std::tie(id, ts, value) = suite.dbcon->results.at(0);
        BOOST_REQUIRE_EQUAL(id, 1);
        BOOST_REQUIRE_EQUAL(ts, 2);
        BOOST_REQUIRE_CLOSE_FRACTION(value, 3.14, 0.00001);
    });
}


BOOST_AUTO_TEST_CASE(Test_tcp_server_loopback_2) {

    TCPServerTestSuite<DbMock> suite;

    suite.run([&](SocketT& socket) {
        boost::asio::streambuf stream;
        std::ostream os(&stream);
        os << ":1\r\n" << ":2\r\n";
        size_t n = boost::asio::write(socket, stream);
        stream.consume(n);

        // Process first part of the message
        suite.io.run_one();

        os << "+3.14\r\n";
        n = boost::asio::write(socket, stream);
        // Process last
        suite.io.run_one();
        suite.pline->stop();

        // Check
        BOOST_REQUIRE_EQUAL(suite.dbcon->results.size(), 1);
        aku_ParamId id;
        aku_Timestamp ts;
        double value;
        std::tie(id, ts, value) = suite.dbcon->results.at(0);
        BOOST_REQUIRE_EQUAL(id, 1);
        BOOST_REQUIRE_EQUAL(ts, 2);
        BOOST_REQUIRE_CLOSE_FRACTION(value, 3.14, 0.00001);
    });
}


BOOST_AUTO_TEST_CASE(Test_tcp_server_loopback_3) {

    TCPServerTestSuite<DbMock> suite;

    suite.run([&](SocketT& socket) {
        boost::asio::streambuf stream;
        std::ostream os(&stream);

        // Fist message
        os << ":1\r\n" << ":2\r\n" << "+3.14\r\n";
        size_t n = boost::asio::write(socket, stream);
        stream.consume(n);

        // Process first part of the message
        suite.io.run_one();

        // Second message
        os << ":3\r\n" << ":4\r\n" << "+1.61\r\n";
        n = boost::asio::write(socket, stream);

        // Process last
        suite.io.run_one();
        suite.pline->stop();

        // Check
        BOOST_REQUIRE_EQUAL(suite.dbcon->results.size(), 2);
        aku_ParamId id;
        aku_Timestamp ts;
        double value;

        // First message
        std::tie(id, ts, value) = suite.dbcon->results.at(0);
        BOOST_REQUIRE_EQUAL(id, 1);
        BOOST_REQUIRE_EQUAL(ts, 2);
        BOOST_REQUIRE_CLOSE_FRACTION(value, 3.14, 0.00001);

        // Second message
        std::tie(id, ts, value) = suite.dbcon->results.at(1);
        BOOST_REQUIRE_EQUAL(id, 3);
        BOOST_REQUIRE_EQUAL(ts, 4);
        BOOST_REQUIRE_CLOSE_FRACTION(value, 1.61, 0.00001);
    });
}


BOOST_AUTO_TEST_CASE(Test_tcp_server_parser_error_handling) {

    TCPServerTestSuite<DbMock> suite;

    suite.run([&](SocketT& socket) {
        boost::asio::streambuf stream;
        std::ostream os(&stream);
        os << ":1\r\n:E\r\n+3.14\r\n";
        //      error ^

        boost::asio::streambuf instream;
        std::istream is(&instream);
        boost::asio::write(socket, stream);

        bool handler_called = false;
        auto cb = [&](boost::system::error_code err) {
            BOOST_REQUIRE(err == boost::asio::error::eof);
            handler_called = true;
        };
        boost::asio::async_read(socket, instream, boost::bind<void>(cb, boost::asio::placeholders::error));

        // TCPSession.handle_read
        suite.io.run_one();  // run message handler (should send error back to us)
        while(!handler_called) {
            suite.io.run_one();  // run error handler
        }

        BOOST_REQUIRE(handler_called);
        // Check
        BOOST_REQUIRE_EQUAL(suite.dbcon->results.size(), 0);
        char buffer[0x1000];
        is.getline(buffer, 0x1000);
        BOOST_REQUIRE_EQUAL(std::string(buffer, buffer + 7), "-PARSER");
        is.getline(buffer, 0x1000);
        BOOST_REQUIRE_EQUAL(std::string(buffer, buffer + 7), "-PARSER");
    });
}


BOOST_AUTO_TEST_CASE(Test_tcp_server_backend_error_handling) {

    TCPServerTestSuite<DbErrMock<AKU_EBAD_DATA>> suite;

    suite.run([&](SocketT& socket) {
        boost::asio::streambuf stream;
        std::ostream os(&stream);
        os << ":1\r\n:2\r\n+3.14\r\n";

        boost::asio::streambuf instream;
        std::istream is(&instream);
        boost::asio::write(socket, stream);

        bool handler_called = false;
        auto cb = [&](boost::system::error_code err) {
            BOOST_REQUIRE(err == boost::asio::error::eof);
            handler_called = true;
        };
        boost::asio::async_read(socket, instream, boost::bind<void>(cb, boost::asio::placeholders::error));

        // TCPSession.handle_read
        suite.io.run_one();  // run message handler (should send error back to us)
        while(!handler_called) {
            suite.io.run_one();  // run error handler
        }

        BOOST_REQUIRE(handler_called);
        // Check
        char buffer[0x1000];
        is.getline(buffer, 0x1000);
        BOOST_REQUIRE_EQUAL(std::string(buffer, buffer + 3), "-DB");
    });
}
