#include "rest/rest_transport.h"
#include "rest/rest_client.h"
#include "rest/http2_client.h"

namespace lt {

std::unique_ptr<IRestTransport> make_rest_transport(
    boost::asio::io_context& ioc,
    const RestClientConfig& config,
    bool use_http2,
    int max_concurrent_streams) {

    if (use_http2) {
        Http2ClientConfig h2_cfg;
        h2_cfg.host = config.host;
        h2_cfg.port = config.port;
        h2_cfg.request_timeout_ms = config.request_timeout_ms;
        h2_cfg.max_concurrent_streams = max_concurrent_streams;
        h2_cfg.reconnect_base_ms = config.reconnect_base_ms;
        h2_cfg.reconnect_max_ms = config.reconnect_max_ms;
        return std::make_unique<Http2Client>(ioc, h2_cfg);
    }

    // Wrap RestClient in an adapter that implements IRestTransport
    class RestClientAdapter : public IRestTransport {
    public:
        RestClientAdapter(boost::asio::io_context& ioc, const RestClientConfig& cfg)
            : client_(ioc, cfg) {}

        void async_send(const RestRequest& req, const L2Headers& headers, RestCallback cb) override {
            client_.async_send(req, headers, std::move(cb));
        }

        bool is_connected() const override { return client_.is_connected(); }
        void request_shutdown() override { client_.request_shutdown(); }

    private:
        RestClient client_;
    };

    return std::make_unique<RestClientAdapter>(ioc, config);
}

}  // namespace lt
