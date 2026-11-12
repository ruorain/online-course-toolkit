// 极简 WebSocket 客户端（只实现文本帧、ping/pong、分片），基于 Winsock。
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <string>

class WsClient {
public:
    WsClient() = default;
    ~WsClient();

    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // host 只支持 IP/主机名 + 明文 ws://（本程序只连 127.0.0.1）
    bool connect(const std::string& host, int port, const std::string& path, int timeout_ms,
                 std::string* err);
    bool sendText(const std::string& payload);
    // 返回 1=收到一条消息, 0=超时, -1=连接断开/出错
    int recvText(std::string& out, int timeout_ms);
    void close();
    bool alive() const { return sock_ != INVALID_SOCKET; }

private:
    int fill(int timeout_ms);
    // 1=已消费一帧(hasMsg 表示是否文本消息), 0=数据不足, -1=协议错误
    int parseOne(std::string& msg, bool& hasMsg, bool& closed);
    void sendFrame(int opcode, const void* data, size_t len);

    SOCKET sock_ = INVALID_SOCKET;
    std::string buf_;
    std::string fragments_;
    std::string host_;
    int port_ = 0;
};
