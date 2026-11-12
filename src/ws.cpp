#include "ws.h"

#include <cstdio>
#include <cstring>
#include <random>

namespace {

const char* kBase64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const unsigned char* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kBase64Chars[(v >> 18) & 63]);
        out.push_back(kBase64Chars[(v >> 12) & 63]);
        out.push_back(kBase64Chars[(v >> 6) & 63]);
        out.push_back(kBase64Chars[v & 63]);
    }
    if (i < len) {
        unsigned v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        out.push_back(kBase64Chars[(v >> 18) & 63]);
        out.push_back(kBase64Chars[(v >> 12) & 63]);
        out.push_back(i + 1 < len ? kBase64Chars[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

long long nowMs() {
    return static_cast<long long>(GetTickCount64());
}

}  // namespace

WsClient::~WsClient() { close(); }

void WsClient::close() {
    if (sock_ != INVALID_SOCKET) {
        ::shutdown(sock_, SD_BOTH);
        ::closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    buf_.clear();
    fragments_.clear();
}

bool WsClient::connect(const std::string& host, int port, const std::string& path,
                       int timeout_ms, std::string* err) {
    close();
    host_ = host;
    port_ = port;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char portText[16];
    std::snprintf(portText, sizeof(portText), "%d", port);
    if (::getaddrinfo(host.c_str(), portText, &hints, &res) != 0 || !res) {
        if (err) *err = "解析地址失败";
        return false;
    }
    SOCKET s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        ::freeaddrinfo(res);
        if (err) *err = "创建 socket 失败";
        return false;
    }
    // 连接超时：先设为非阻塞，用 select 等
    u_long nonblock = 1;
    ::ioctlsocket(s, FIONBIO, &nonblock);
    int rc = ::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    ::freeaddrinfo(res);
    if (rc != 0 && ::WSAGetLastError() != WSAEWOULDBLOCK) {
        ::closesocket(s);
        if (err) *err = "连接被拒绝";
        return false;
    }
    if (rc != 0) {
        fd_set wr;
        FD_ZERO(&wr);
        FD_SET(s, &wr);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (::select(0, nullptr, &wr, nullptr, &tv) <= 0) {
            ::closesocket(s);
            if (err) *err = "连接超时";
            return false;
        }
    }
    nonblock = 0;
    ::ioctlsocket(s, FIONBIO, &nonblock);
    sock_ = s;

    unsigned char nonce[16];
    std::random_device rd;
    for (auto& c : nonce) c = static_cast<unsigned char>(rd());

    std::string req;
    req.reserve(256);
    req += "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + ":" + portText + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + base64(nonce, sizeof(nonce)) + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n\r\n";
    if (::send(sock_, req.data(), static_cast<int>(req.size()), 0) <= 0) {
        close();
        if (err) *err = "发送握手失败";
        return false;
    }

    // 读响应头
    std::string head;
    long long deadline = nowMs() + timeout_ms;
    while (head.find("\r\n\r\n") == std::string::npos) {
        if (nowMs() > deadline) {
            close();
            if (err) *err = "握手超时";
            return false;
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(sock_, &rd);
        timeval tv{0, 100 * 1000};
        if (::select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;
        char tmp[2048];
        int n = ::recv(sock_, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            close();
            if (err) *err = "握手被关闭";
            return false;
        }
        head.append(tmp, static_cast<size_t>(n));
        if (head.size() > 64 * 1024) break;
    }
    size_t split = head.find("\r\n\r\n");
    if (split == std::string::npos || head.find(" 101 ") == std::string::npos) {
        close();
        if (err) *err = "握手被拒绝";
        return false;
    }
    buf_ = head.substr(split + 4);
    return true;
}

int WsClient::fill(int timeout_ms) {
    if (sock_ == INVALID_SOCKET) return -1;
    if (timeout_ms < 0) timeout_ms = 0;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(sock_, &rd);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int sel = ::select(0, &rd, nullptr, nullptr, &tv);
    if (sel == 0) return 0;
    if (sel < 0) return -1;
    char tmp[16384];
    int n = ::recv(sock_, tmp, sizeof(tmp), 0);
    if (n <= 0) {
        close();
        return -1;
    }
    buf_.append(tmp, static_cast<size_t>(n));
    return 1;
}

void WsClient::sendFrame(int opcode, const void* data, size_t len) {
    if (sock_ == INVALID_SOCKET) return;
    std::string out;
    out.reserve(len + 14);
    out.push_back(static_cast<char>(0x80 | (opcode & 0x0F)));

    unsigned char mask[4];
    std::random_device rd;
    for (auto& m : mask) m = static_cast<unsigned char>(rd());

    if (len < 126) {
        out.push_back(static_cast<char>(0x80 | len));
    } else if (len < 65536) {
        out.push_back(static_cast<char>(0x80 | 126));
        out.push_back(static_cast<char>((len >> 8) & 0xFF));
        out.push_back(static_cast<char>(len & 0xFF));
    } else {
        out.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
    }
    out.append(reinterpret_cast<const char*>(mask), 4);
    const unsigned char* src = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(static_cast<char>(src[i] ^ mask[i % 4]));
    }
    ::send(sock_, out.data(), static_cast<int>(out.size()), 0);
}

bool WsClient::sendText(const std::string& payload) {
    if (sock_ == INVALID_SOCKET) return false;
    sendFrame(0x1, payload.data(), payload.size());
    return true;
}

int WsClient::parseOne(std::string& msg, bool& hasMsg, bool& closed) {
    hasMsg = false;
    closed = false;
    if (buf_.size() < 2) return 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(buf_.data());
    bool fin = (p[0] & 0x80) != 0;
    int opcode = p[0] & 0x0F;
    bool masked = (p[1] & 0x80) != 0;
    uint64_t len = p[1] & 0x7F;
    size_t pos = 2;
    if (len == 126) {
        if (buf_.size() < 4) return 0;
        len = (static_cast<uint64_t>(p[2]) << 8) | p[3];
        pos = 4;
    } else if (len == 127) {
        if (buf_.size() < 10) return 0;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
        pos = 10;
    }
    if (len > 64u * 1024u * 1024u) return -1;
    size_t maskPos = pos;
    if (masked) pos += 4;
    if (buf_.size() < pos + len) return 0;

    std::string payload(buf_.data() + pos, static_cast<size_t>(len));
    if (masked) {
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(payload[i] ^ buf_[maskPos + (i % 4)]);
        }
    }
    buf_.erase(0, pos + static_cast<size_t>(len));

    if (opcode == 0x9) {  // ping
        sendFrame(0xA, payload.data(), payload.size());
        return 1;
    }
    if (opcode == 0xA) return 1;  // pong
    if (opcode == 0x8) {
        closed = true;
        return 1;
    }
    if (opcode == 0x0) {
        fragments_ += payload;
        if (!fin) return 1;
        msg = fragments_;
        fragments_.clear();
        hasMsg = true;
        return 1;
    }
    if (!fin) {
        fragments_ = payload;
        return 1;
    }
    msg = payload;
    hasMsg = true;
    return 1;
}

int WsClient::recvText(std::string& out, int timeout_ms) {
    long long deadline = nowMs() + timeout_ms;
    for (;;) {
        std::string msg;
        bool hasMsg = false, closed = false;
        int r = parseOne(msg, hasMsg, closed);
        if (r < 0) return -1;
        if (closed) {
            close();
            return -1;
        }
        if (hasMsg) {
            out = std::move(msg);
            return 1;
        }
        long long left = deadline - nowMs();
        if (left <= 0) return 0;
        int f = fill(static_cast<int>(left > 250 ? 250 : left));
        if (f < 0) return -1;
    }
}
