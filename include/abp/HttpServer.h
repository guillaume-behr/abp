#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace abp::http {

/// Percent-decodes a URL component, turning "+" into a space (the encoding
/// browsers use for query strings).
std::string urlDecode(const std::string& text);

/// Parses "a=1&b=two%20words" into decoded key/value pairs. Keys without a
/// value get an empty string.
std::vector<std::pair<std::string, std::string>> parseQuery(const std::string& query);

/// One parsed HTTP request. Bodies are held in memory, which is fine
/// because every request this server accepts is a small JSON command --
/// large payloads are rejected before they are read.
struct Request {
    std::string method;
    std::string target; ///< Raw request target, e.g. "/api/job?since=3".
    std::string path;   ///< Decoded path portion, e.g. "/api/job".
    std::string query;  ///< Raw query string, without the leading '?'.
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    /// Case-insensitive header lookup; returns an empty string if absent.
    std::string header(const std::string& name) const;

    /// Decoded value of a query parameter, or `def` if it is not present.
    std::string param(const std::string& name, const std::string& def = "") const;
};

struct Response {
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    /// When set, the body is `fileLength` bytes of this file starting at
    /// `fileOffset`, streamed from disk rather than held in memory -- a
    /// backup's video can be gigabytes. `body` is ignored. A `Range` request
    /// header is honoured within that segment.
    std::string filePath;
    unsigned long long fileOffset = 0;
    unsigned long long fileLength = 0;

    static Response json(const std::string& body, int status = 200);
    static Response html(const std::string& body, int status = 200);
    static Response text(const std::string& body, int status = 200);
    static Response file(const std::string& path, unsigned long long offset, unsigned long long length,
                         const std::string& contentType);
};

enum class RangeResult {
    None,          ///< No usable Range header: send the whole thing.
    Satisfiable,   ///< Send *start / *length.
    Unsatisfiable, ///< Answer 416.
};

/// Interprets a single-range `Range: bytes=...` header against a body of
/// `total` bytes ("a-b", "a-" and "-suffix" forms). Multiple ranges are not
/// supported and read as None, which is allowed: the server may always
/// answer with the full body.
RangeResult parseByteRange(const std::string& header, unsigned long long total, unsigned long long* start,
                           unsigned long long* length);

/// A tiny, dependency-free HTTP/1.1 server for the local web GUI. It binds
/// a listening socket (loopback by default), then serves each accepted
/// connection on its own thread, so a long poll from one browser tab never
/// blocks another.
///
/// This is deliberately not a general-purpose web server: it speaks just
/// enough HTTP for a single-page app talking to a JSON API on the same
/// machine (no keep-alive, no chunked request bodies, no TLS).
class HttpServer {
public:
    using Handler = std::function<Response(const Request&)>;

    HttpServer() = default;
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /// Binds and starts listening. `port` may be 0 to let the OS choose a
    /// free port, which boundPort() then reports. On failure, returns false
    /// and sets `*error` (if given) to a human-readable reason.
    bool listen(const std::string& host, int port, std::string* error = nullptr);

    /// The port actually bound, or 0 if listen() has not succeeded.
    int boundPort() const { return boundPort_; }

    /// Accepts connections until stop() is called, dispatching each request
    /// to `handler`. `handler` is called from multiple threads and must be
    /// thread-safe.
    void serveForever(Handler handler);

    /// Makes serveForever() return. Safe to call from a request handler or
    /// a signal-handling thread.
    void stop();

private:
    std::atomic<int> listenFd_{-1};
    /// Written by listen() and read by whoever prints the URL, which on the
    /// GUI's path is a different thread.
    std::atomic<int> boundPort_{0};
    std::atomic<bool> stopping_{false};
};

} // namespace abp::http
