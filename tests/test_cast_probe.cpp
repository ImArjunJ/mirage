#include <iostream>
#include <string>

#include "protocols/cast/probe.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool contains(const std::string& value, const char* needle) {
    return value.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    bool ok = true;

    auto options = mirage::protocols::cast::handle_probe(
        "OPTIONS /setup/eureka_info HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", "Living Room");
    ok &= expect(options.kind == mirage::protocols::cast::probe_kind::http_options,
                 "options probe kind mismatch");
    ok &= expect(contains(options.response, "HTTP/1.1 204 No Content"),
                 "options response status mismatch");
    ok &= expect(contains(options.response, "Allow: GET, HEAD, OPTIONS"),
                 "options response allow header mismatch");

    auto get = mirage::protocols::cast::handle_probe(
        "GET /setup/eureka_info HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", "Living Room");
    ok &= expect(get.kind == mirage::protocols::cast::probe_kind::http_status,
                 "get probe kind mismatch");
    ok &= expect(contains(get.response, "HTTP/1.1 501 Not Implemented"),
                 "get response status mismatch");
    ok &= expect(contains(get.response, "\"name\":\"Living Room\""),
                 "get response name mismatch");
    ok &= expect(contains(get.response, "\"status\":\"not_implemented\""),
                 "get response status body mismatch");
    ok &= expect(contains(get.response, "cast media channel is not implemented yet"),
                 "get response detail mismatch");

    auto head = mirage::protocols::cast::handle_probe(
        "HEAD /setup/eureka_info HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", "Living Room");
    ok &= expect(head.kind == mirage::protocols::cast::probe_kind::http_status,
                 "head probe kind mismatch");
    ok &= expect(contains(head.response, "Content-Length: 0"), "head content length mismatch");
    ok &= expect(!contains(head.response, "\"status\":\"not_implemented\""),
                 "head response unexpectedly included a body");

    std::string tls_client_hello;
    tls_client_hello.push_back(static_cast<char>(0x16));
    tls_client_hello.push_back(static_cast<char>(0x03));
    tls_client_hello.push_back(static_cast<char>(0x01));
    auto tls = mirage::protocols::cast::handle_probe(tls_client_hello, "Living Room");
    ok &= expect(tls.kind == mirage::protocols::cast::probe_kind::tls_client_hello,
                 "tls probe kind mismatch");
    ok &= expect(tls.response.empty(), "tls probe should not send plaintext");

    auto unsupported = mirage::protocols::cast::handle_probe("not cast", "Living Room");
    ok &= expect(unsupported.kind == mirage::protocols::cast::probe_kind::unsupported,
                 "unsupported probe kind mismatch");
    ok &= expect(unsupported.response.empty(), "unsupported probe should not send a response");

    return ok ? 0 : 1;
}
