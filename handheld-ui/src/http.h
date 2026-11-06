// http.h — tiny blocking HTTP/1.1 to the handheld supervisor on 127.0.0.1 (the /handheld/* endpoints).
// ★ Connection: close, one request per connection; the body is read to EOF. Only ever called from worker threads
//   (never the draw loop), with a timeout, so a busy engine never stalls a frame.
#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <string>

namespace hh {

struct HttpResult {
    int status = 0;          // 0 = no answer
    std::string body;
};

inline HttpResult httpRequest(int port, const std::string& method, const std::string& path,
                              const std::string& body = "", int timeoutSec = 5) {
    HttpResult res;
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return res;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    timeval tv{timeoutSec, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (::connect(fd, (sockaddr*)&a, sizeof a) != 0) { ::close(fd); return res; }
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\nCache-Control: no-store\r\n";
    if (!body.empty() || method == "POST")
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    const char* p = req.data();
    size_t left = req.size();
    while (left) {
        const ssize_t w = ::send(fd, p, left, MSG_NOSIGNAL);
        if (w <= 0) { ::close(fd); return res; }
        p += w; left -= (size_t)w;
    }
    std::string all;
    char buf[8192];
    for (;;) {
        const ssize_t r = ::recv(fd, buf, sizeof buf, 0);
        if (r <= 0) break;
        all.append(buf, (size_t)r);
        if (all.size() > (4u << 20)) break;
    }
    ::close(fd);
    if (all.compare(0, 5, "HTTP/") != 0) return res;
    const size_t sp = all.find(' ');
    if (sp != std::string::npos) res.status = std::atoi(all.c_str() + sp + 1);
    const size_t he = all.find("\r\n\r\n");
    if (he == std::string::npos) return res;
    const std::string head = all.substr(0, he);
    std::string b = all.substr(he + 4);
    // ★ chunked bodies (the engine uses Content-Length today; decode anyway, so a change there cannot break us)
    std::string lower = head;
    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
    if (lower.find("transfer-encoding: chunked") != std::string::npos) {
        std::string out;
        size_t i = 0;
        while (i < b.size()) {
            const size_t e = b.find("\r\n", i);
            if (e == std::string::npos) break;
            const size_t n = std::strtoul(b.c_str() + i, nullptr, 16);
            if (n == 0) break;
            out.append(b, e + 2, n);
            i = e + 2 + n + 2;
        }
        b.swap(out);
    }
    res.body.swap(b);
    return res;
}

}  // namespace hh
