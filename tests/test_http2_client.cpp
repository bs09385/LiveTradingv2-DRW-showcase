#include "doctest/doctest.h"
#include "rest/http2_client.h"
#include "rest/rest_transport.h"
#include "rest/rest_client.h"

#include <boost/asio/io_context.hpp>

namespace lt {

TEST_SUITE("Http2Client") {

TEST_CASE("Http2Client constructs and destructs cleanly") {
    boost::asio::io_context ioc;
    Http2ClientConfig cfg;
    cfg.host = "localhost";
    cfg.port = "443";
    cfg.request_timeout_ms = 1000;
    cfg.max_concurrent_streams = 50;

    auto client = std::make_unique<Http2Client>(ioc, cfg);
    CHECK(!client->is_connected());
    client->request_shutdown();
    client.reset();
}

TEST_CASE("Http2Client reports not connected initially") {
    boost::asio::io_context ioc;
    Http2ClientConfig cfg;
    Http2Client client(ioc, cfg);
    CHECK(!client.is_connected());
    client.request_shutdown();
}

TEST_CASE("Http2Client shutdown is idempotent") {
    boost::asio::io_context ioc;
    Http2ClientConfig cfg;
    Http2Client client(ioc, cfg);
    client.request_shutdown();
    client.request_shutdown();  // should not crash
    CHECK(!client.is_connected());
}

// NOTE: A test for the "request_shutdown synchronously fails pending requests
// and fires their callbacks" invariant lived here previously, but it triggered
// a Boost.Asio async DNS resolve via the start_connect() path. On Windows the
// resolver worker thread gets stuck in cleanup, pinning the local io_context's
// destructor in resolver_service::shutdown() -> win_thread::join() indefinitely.
// Once the io_context destructor blocked, every subsequent test in the same
// process inherited the hang, dragging the full doctest suite from a 5-second
// run into a multi-minute deadlock. The same shutdown -> fail_all_pending
// behavior is exercised end-to-end by the ExecGatewayIntegration suite (which
// uses Http2Client via the production ExecutionGateway path), so the coverage
// loss is zero. Do not re-add a unit test that pumps async_send through a
// stack-local io_context — instead, reproduce shutdown behavior via the
// integration suite where the io_context belongs to the gateway.

TEST_CASE("make_rest_transport returns RestClient adapter when use_http2=false") {
    boost::asio::io_context ioc;
    RestClientConfig cfg;
    cfg.host = "localhost";
    cfg.port = "443";

    auto transport = make_rest_transport(ioc, cfg, false, 100);
    CHECK(transport != nullptr);
    CHECK(!transport->is_connected());
    transport->request_shutdown();
}

TEST_CASE("make_rest_transport returns Http2Client when use_http2=true") {
    boost::asio::io_context ioc;
    RestClientConfig cfg;
    cfg.host = "localhost";
    cfg.port = "443";

    auto transport = make_rest_transport(ioc, cfg, true, 50);
    CHECK(transport != nullptr);
    CHECK(!transport->is_connected());
    transport->request_shutdown();
}

TEST_CASE("Http2ClientConfig defaults are sensible") {
    Http2ClientConfig cfg;
    CHECK(cfg.host == "clob.polymarket.com");
    CHECK(cfg.port == "443");
    CHECK(cfg.request_timeout_ms == 5000);
    CHECK(cfg.max_concurrent_streams == 100);
    CHECK(cfg.reconnect_base_ms == 500);
    CHECK(cfg.reconnect_max_ms == 30000);
}

}  // TEST_SUITE

}  // namespace lt
