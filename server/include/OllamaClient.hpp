#pragma once

#include <functional>
#include <memory>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace mom {

// Minimal one-shot async HTTP client for Ollama's local /api/chat endpoint.
// Every call opens its own connection and runs entirely on the caller's
// io_context — no extra thread — so a slow model response never blocks
// other players' WebSocket traffic (docs/DESIGN.md section 4: "외부 I/O는
// 병렬적으로 처리하되, 게임 상태 변경은 통제된 실행 흐름에서 수행한다").
//
// The callback always fires exactly once, with `ok=false` and a short
// reason string for any failure (connect refused, timeout, non-200,
// malformed response) instead of throwing or hanging — callers should
// treat that as "AI unavailable" and fall back, not as a program error.
class OllamaClient : public std::enable_shared_from_this<OllamaClient> {
public:
    using ResponseCallback = std::function<void(bool ok, std::string content_or_error)>;

    OllamaClient(boost::asio::io_context& ioc, std::string host, std::string port);

    // Sends a single-turn chat request with format=json (constrains the
    // model to emit syntactically valid JSON) and stream=false (so the
    // whole answer arrives as one HTTP response body). On success, passes
    // the assistant message's raw text content — which the caller must
    // still parse and validate as its own JSON schema, since "valid JSON"
    // doesn't guarantee "the fields we asked for".
    void chat_json(const std::string& model,
                   const std::string& system_prompt,
                   const std::string& user_prompt,
                   ResponseCallback on_done);

private:
    void on_resolve(boost::beast::error_code ec, boost::asio::ip::tcp::resolver::results_type results);
    void on_connect(boost::beast::error_code ec);
    void on_write(boost::beast::error_code ec, std::size_t bytes_transferred);
    void on_read(boost::beast::error_code ec, std::size_t bytes_transferred);
    void fail(boost::beast::error_code ec, const char* what);

    boost::asio::io_context& ioc_;
    std::string host_;
    std::string port_;
    boost::asio::ip::tcp::resolver resolver_;
    boost::beast::tcp_stream stream_;
    boost::beast::flat_buffer buffer_;
    boost::beast::http::request<boost::beast::http::string_body> req_;
    boost::beast::http::response<boost::beast::http::string_body> res_;
    ResponseCallback on_done_;
};

}
