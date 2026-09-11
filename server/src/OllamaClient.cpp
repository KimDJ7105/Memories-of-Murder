#include "OllamaClient.hpp"

#include <chrono>

#include <nlohmann/json.hpp>

namespace mom {

namespace {
namespace beast = boost::beast;
namespace http = beast::http;

// Generous budget covering the whole connect+write+read sequence. Warm
// (already-loaded) 7-8B models on consumer GPUs typically answer in a few
// seconds, but a *cold* model load (weights not yet in VRAM) can itself
// take well over a minute — GameServer warms the model up at startup for
// exactly this reason, but this timeout still needs enough headroom to
// survive a cold call if one slips through, without hanging forever on a
// genuinely dead Ollama process.
constexpr auto kTimeout = std::chrono::seconds(120);
}

OllamaClient::OllamaClient(boost::asio::io_context& ioc, std::string host, std::string port)
    : ioc_(ioc)
    , host_(std::move(host))
    , port_(std::move(port))
    , resolver_(ioc)
    , stream_(ioc)
{
}

void OllamaClient::chat_json(const std::string& model,
                              const std::string& system_prompt,
                              const std::string& user_prompt,
                              ResponseCallback on_done)
{
    on_done_ = std::move(on_done);

    // Built via nlohmann::json rather than string concatenation so the
    // crime/guess text (which may contain quotes, newlines, backslashes)
    // is always escaped correctly — never hand-interpolate user text into
    // a JSON literal.
    const nlohmann::json body = {
        {"model", model},
        {"stream", false},
        {"format", "json"},
        {"messages", nlohmann::json::array({
             {{"role", "system"}, {"content", system_prompt}},
             {{"role", "user"}, {"content", user_prompt}},
         })},
    };

    req_ = {};
    req_.method(http::verb::post);
    req_.target("/api/chat");
    req_.version(11);
    req_.set(http::field::host, host_);
    req_.set(http::field::content_type, "application/json");
    req_.keep_alive(false);
    req_.body() = body.dump();
    req_.prepare_payload();

    buffer_.clear();
    res_ = {};

    stream_.expires_after(kTimeout);

    auto self = shared_from_this();
    resolver_.async_resolve(host_, port_,
        [this, self](beast::error_code ec, boost::asio::ip::tcp::resolver::results_type results) {
            on_resolve(ec, results);
        });
}

void OllamaClient::on_resolve(beast::error_code ec, boost::asio::ip::tcp::resolver::results_type results)
{
    if (ec) { fail(ec, "resolve"); return; }

    auto self = shared_from_this();
    stream_.async_connect(results, [this, self](beast::error_code ec, boost::asio::ip::tcp::resolver::results_type::endpoint_type) {
        on_connect(ec);
    });
}

void OllamaClient::on_connect(beast::error_code ec)
{
    if (ec) { fail(ec, "connect"); return; }

    auto self = shared_from_this();
    http::async_write(stream_, req_, [this, self](beast::error_code ec, std::size_t bytes) {
        on_write(ec, bytes);
    });
}

void OllamaClient::on_write(beast::error_code ec, std::size_t)
{
    if (ec) { fail(ec, "write"); return; }

    auto self = shared_from_this();
    http::async_read(stream_, buffer_, res_, [this, self](beast::error_code ec, std::size_t bytes) {
        on_read(ec, bytes);
    });
}

void OllamaClient::on_read(beast::error_code ec, std::size_t)
{
    beast::error_code ignored;
    stream_.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);

    if (ec) { fail(ec, "read"); return; }

    if (res_.result() != http::status::ok) {
        on_done_(false, "Ollama HTTP " + std::to_string(res_.result_int()) + ": " + res_.body());
        return;
    }

    // Unwrap Ollama's envelope ({"message":{"content":"..."},...}) so
    // callers only ever deal with the model's own JSON text, not the
    // transport format around it.
    try {
        const nlohmann::json outer = nlohmann::json::parse(res_.body());
        const std::string content = outer.at("message").at("content").get<std::string>();
        on_done_(true, content);
    } catch (const std::exception& e) {
        on_done_(false, std::string("malformed Ollama response envelope: ") + e.what());
    }
}

void OllamaClient::fail(beast::error_code ec, const char* what)
{
    on_done_(false, std::string(what) + " failed: " + ec.message());
}

}
