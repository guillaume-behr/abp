#include "abp/HttpServer.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

#include "abp/Logger.h"
#include "abp/StringUtil.h"

namespace abp::http {
namespace {

constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr size_t kMaxBodyBytes = 4 * 1024 * 1024;
constexpr int kSocketTimeoutSeconds = 30;

int hexDigitValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string toLower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

const char* statusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: break;
    }
    // A reason phrase is advisory, but calling an unlisted status "OK" would
    // put "503 OK" on the wire. Fall back to the class instead.
    if (status >= 500) return "Server Error";
    if (status >= 400) return "Client Error";
    if (status >= 300) return "Redirection";
    if (status >= 200) return "Success";
    return "Informational";
}

bool writeAll(int fd, const char* data, size_t size) {
    size_t written = 0;
    while (written < size) {
        ssize_t n = ::write(fd, data + written, size - written);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

/// Reads one whole request off `fd`. Returns false if the request could not
/// be served, setting *errorStatus to the status to report back -- or to 0
/// when the peer simply hung up and there is nothing to reply to.
bool readRequest(int fd, Request* request, int* errorStatus) {
    *errorStatus = 0;

    std::string buffer;
    char chunk[4096];
    size_t headerEnd = std::string::npos;

    while (true) {
        headerEnd = buffer.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
        ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        buffer.append(chunk, static_cast<size_t>(n));
        // Checked after appending, not before: checking first would let the
        // buffer reach the cap *plus* one whole read before anyone noticed.
        if (buffer.size() > kMaxHeaderBytes) {
            *errorStatus = 413;
            return false;
        }
    }

    std::string head = buffer.substr(0, headerEnd);
    std::string rest = buffer.substr(headerEnd + 4);

    std::vector<std::string> lines = strutil::split(head, '\n');
    std::vector<std::string> requestLine = strutil::split(strutil::trim(lines.empty() ? "" : lines[0]), ' ');
    if (requestLine.size() < 2) {
        *errorStatus = 400;
        return false;
    }
    request->method = requestLine[0];
    request->target = requestLine[1];

    size_t questionMark = request->target.find('?');
    if (questionMark == std::string::npos) {
        request->path = urlDecode(request->target);
    } else {
        request->path = urlDecode(request->target.substr(0, questionMark));
        request->query = request->target.substr(questionMark + 1);
    }

    size_t contentLength = 0;
    for (size_t i = 1; i < lines.size(); ++i) {
        std::string line = strutil::trim(lines[i]);
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = strutil::trim(line.substr(0, colon));
        std::string value = strutil::trim(line.substr(colon + 1));
        if (toLower(name) == "content-length") {
            try {
                long long parsed = std::stoll(value);
                if (parsed < 0) {
                    *errorStatus = 400;
                    return false;
                }
                contentLength = static_cast<size_t>(parsed);
            } catch (const std::exception&) {
                *errorStatus = 400;
                return false;
            }
        }
        request->headers.emplace_back(std::move(name), std::move(value));
    }

    if (contentLength > kMaxBodyBytes) {
        *errorStatus = 413;
        return false;
    }

    request->body = std::move(rest);
    while (request->body.size() < contentLength) {
        ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        request->body.append(chunk, static_cast<size_t>(n));
    }
    request->body.resize(contentLength);
    return true;
}

void sendResponse(int fd, const Response& response) {
    std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " + statusText(response.status) + "\r\n";
    out += "Content-Type: " + response.contentType + "\r\n";
    out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\n";
    // The GUI is a local, single-origin app: nothing it serves should be
    // content-sniffed or embedded in someone else's page.
    out += "X-Content-Type-Options: nosniff\r\n";
    out += "X-Frame-Options: DENY\r\n";
    out += "Connection: close\r\n";
    for (const auto& header : response.headers) {
        out += header.first + ": " + header.second + "\r\n";
    }
    out += "\r\n";

    if (!writeAll(fd, out.data(), out.size())) return;
    writeAll(fd, response.body.data(), response.body.size());
}

void setSocketTimeout(int fd) {
    timeval timeout{};
    timeout.tv_sec = kSocketTimeoutSeconds;
    timeout.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

} // namespace

std::string urlDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < text.size()) {
            int high = hexDigitValue(text[i + 1]);
            int low = hexDigitValue(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
            } else {
                out.push_back(c);
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> parseQuery(const std::string& query) {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& field : strutil::split(query, '&')) {
        if (field.empty()) continue;
        size_t equals = field.find('=');
        if (equals == std::string::npos) {
            result.emplace_back(urlDecode(field), std::string());
        } else {
            result.emplace_back(urlDecode(field.substr(0, equals)), urlDecode(field.substr(equals + 1)));
        }
    }
    return result;
}

std::string Request::header(const std::string& name) const {
    std::string wanted = toLower(name);
    for (const auto& entry : headers) {
        if (toLower(entry.first) == wanted) return entry.second;
    }
    return std::string();
}

std::string Request::param(const std::string& name, const std::string& def) const {
    for (const auto& entry : parseQuery(query)) {
        if (entry.first == name) return entry.second;
    }
    return def;
}

Response Response::json(const std::string& body, int status) {
    Response response;
    response.status = status;
    response.contentType = "application/json; charset=utf-8";
    response.body = body;
    return response;
}

Response Response::html(const std::string& body, int status) {
    Response response;
    response.status = status;
    response.contentType = "text/html; charset=utf-8";
    response.body = body;
    return response;
}

Response Response::text(const std::string& body, int status) {
    Response response;
    response.status = status;
    response.contentType = "text/plain; charset=utf-8";
    response.body = body;
    return response;
}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::listen(const std::string& host, int port, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message + ": " + std::strerror(errno);
        return false;
    };

    if (port < 0 || port > 65535) {
        if (error != nullptr) *error = "Port out of range: " + std::to_string(port);
        return false;
    }

    // A browser that goes away mid-response must not take the server down.
    std::signal(SIGPIPE, SIG_IGN);

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return fail("Could not create socket");

    int enable = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));

    std::string bindHost = (host == "localhost") ? "127.0.0.1" : host;
    if (::inet_pton(AF_INET, bindHost.c_str(), &address.sin_addr) != 1) {
        ::close(fd);
        if (error != nullptr) *error = "Not a valid IPv4 address to bind to: " + host;
        return false;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::string message = "Could not bind " + bindHost + ":" + std::to_string(port);
        ::close(fd);
        return fail(message);
    }

    if (::listen(fd, 32) != 0) {
        ::close(fd);
        return fail("Could not listen on socket");
    }

    sockaddr_in bound{};
    socklen_t boundSize = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundSize) == 0) {
        boundPort_ = ntohs(bound.sin_port);
    } else {
        boundPort_ = port;
    }

    listenFd_ = fd;
    stopping_ = false;
    return true;
}

void HttpServer::serveForever(Handler handler) {
    while (!stopping_) {
        int fd = listenFd_;
        if (fd < 0) break;

        int client = ::accept(fd, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) continue;
            if (stopping_) break;
            Logger::debug(std::string("accept() failed: ") + std::strerror(errno));
            continue;
        }

        auto serve = [client, handler]() {
            setSocketTimeout(client);
            Request request;
            int errorStatus = 0;
            if (readRequest(client, &request, &errorStatus)) {
                Response response;
                try {
                    response = handler(request);
                } catch (const std::exception& e) {
                    Logger::debug(std::string("Request handler threw: ") + e.what());
                    response = Response::text("Internal server error", 500);
                }
                sendResponse(client, response);
            } else if (errorStatus != 0) {
                sendResponse(client, Response::text(statusText(errorStatus), errorStatus));
            }
            ::shutdown(client, SHUT_WR);
            ::close(client);
        };

        // std::thread's constructor throws when the process is out of threads
        // (a browser opening connections faster than they retire, or an
        // exhausted rlimit). Letting that escape would tear down the server
        // and leak this connection, so the request is answered on this thread
        // instead: slower, but the GUI stays up and the socket still closes.
        try {
            std::thread(serve).detach();
        } catch (const std::system_error& e) {
            Logger::debug(std::string("Could not start a request thread, serving inline: ") + e.what());
            serve();
        }
    }
}

void HttpServer::stop() {
    stopping_ = true;
    int fd = listenFd_.exchange(-1);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
}

} // namespace abp::http
