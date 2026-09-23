#include "abp/HttpServer.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <string>

#include "TestFramework.h"

using namespace abp;
using namespace abp::http;

ABP_TEST(url_decode_percent_and_plus) {
    ABP_CHECK_EQ(urlDecode("plain"), std::string("plain"));
    ABP_CHECK_EQ(urlDecode("two+words"), std::string("two words"));
    ABP_CHECK_EQ(urlDecode("%2Fhome%2Fuser"), std::string("/home/user"));
    ABP_CHECK_EQ(urlDecode("a%2Bb"), std::string("a+b"));
    ABP_CHECK_EQ(urlDecode("caf%C3%A9"), std::string("caf\xC3\xA9"));
}

ABP_TEST(url_decode_leaves_broken_escapes_alone) {
    // A stray '%' is data, not a decoding error: paths typed by a human end
    // up here and must not be silently mangled.
    ABP_CHECK_EQ(urlDecode("100%"), std::string("100%"));
    ABP_CHECK_EQ(urlDecode("%zz"), std::string("%zz"));
    ABP_CHECK_EQ(urlDecode("%4"), std::string("%4"));
}

ABP_TEST(parse_query_pairs) {
    auto pairs = parseQuery("path=%2Ftmp%2Fb&sub=apks%2Fcom.example&flag");
    ABP_CHECK_EQ(pairs.size(), static_cast<size_t>(3));
    ABP_CHECK_EQ(pairs[0].first, std::string("path"));
    ABP_CHECK_EQ(pairs[0].second, std::string("/tmp/b"));
    ABP_CHECK_EQ(pairs[1].second, std::string("apks/com.example"));
    ABP_CHECK_EQ(pairs[2].first, std::string("flag"));
    ABP_CHECK_EQ(pairs[2].second, std::string(""));
}

ABP_TEST(parse_query_empty) {
    ABP_CHECK(parseQuery("").empty());
    ABP_CHECK(parseQuery("&&").empty());
}

ABP_TEST(request_header_lookup_is_case_insensitive) {
    Request request;
    request.headers.emplace_back("X-Abp-Token", "secret");
    request.headers.emplace_back("Content-Length", "12");

    ABP_CHECK_EQ(request.header("x-abp-token"), std::string("secret"));
    ABP_CHECK_EQ(request.header("X-ABP-TOKEN"), std::string("secret"));
    ABP_CHECK_EQ(request.header("content-length"), std::string("12"));
    ABP_CHECK_EQ(request.header("missing"), std::string(""));
}

ABP_TEST(request_params_are_decoded) {
    Request request;
    request.query = "serial=ABC123&sub=a%20b";

    ABP_CHECK_EQ(request.param("serial"), std::string("ABC123"));
    ABP_CHECK_EQ(request.param("sub"), std::string("a b"));
    ABP_CHECK_EQ(request.param("absent", "fallback"), std::string("fallback"));
}

ABP_TEST(http_server_sockets_are_not_inherited_by_child_processes) {
    // The GUI spawns adb, and adb's first run forks a long-lived server. A
    // listening socket without close-on-exec follows it into that daemon and
    // keeps the port bound after abp exits.
    auto socketsWithoutCloexec = [] {
        int count = 0;
        for (int fd = 0; fd < 1024; ++fd) {
            struct stat info {};
            if (::fstat(fd, &info) != 0 || !S_ISSOCK(info.st_mode)) continue;
            int flags = ::fcntl(fd, F_GETFD);
            if (flags != -1 && !(flags & FD_CLOEXEC)) ++count;
        }
        return count;
    };

    const int before = socketsWithoutCloexec();
    HttpServer server;
    std::string error;
    ABP_CHECK(server.listen("127.0.0.1", 0, &error));
    ABP_CHECK(server.boundPort() > 0);
    ABP_CHECK_EQ(socketsWithoutCloexec(), before);
    server.stop();
}
