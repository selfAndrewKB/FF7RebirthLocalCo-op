// Host-side tests for control::Server, the console's socket, over real loopback connections (port kTestPort).
// Lines split and joined across sends,
// replies out, a long line and a leaving client reported closed, the replies queued before Close sent, a client
// that stops reading closed, the client limit, a taken port, Close.

#include "control/server.h"

#include "expect.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

using lss::control::Server;
using namespace std::chrono_literals;

// Not the mod's port (4419) nor the other game's (4418): a running game never clashes.
constexpr std::uint16_t kTestPort = 4429;

/// A blocking client socket with a receive timeout; INVALID_SOCKET if the connection was refused.
SOCKET Connect()
{
    const SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in  address{};
    address.sin_family = AF_INET;
    address.sin_port   = htons(kTestPort);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        closesocket(sock);
        return INVALID_SOCKET;
    }
    const DWORD timeout = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return sock;
}

void SendText(SOCKET sock, const std::string& text)
{
    send(sock, text.data(), static_cast<int>(text.size()), 0);
}

/// Drains until `count` events arrived or two seconds passed.
std::vector<Server::Event> Collect(Server& server, std::size_t count)
{
    std::vector<Server::Event> events;
    for (int i = 0; i < 200 && events.size() < count; ++i)
    {
        for (auto& event : server.Drain())
        {
            events.push_back(std::move(event));
        }
        if (events.size() < count)
        {
            std::this_thread::sleep_for(10ms);
        }
    }
    return events;
}

/// Reads until a newline or the timeout; the text without its newline.
std::string ReadLine(SOCKET sock)
{
    std::string line;
    char        c = 0;
    while (recv(sock, &c, 1, 0) == 1 && c != '\n')
    {
        line += c;
    }
    return line;
}

/// The peer closed the connection (recv returns 0) within the timeout.
bool ClosedByPeer(SOCKET sock)
{
    char buffer[256];
    for (;;)
    {
        const int got = recv(sock, buffer, sizeof(buffer), 0);
        if (got == 0)
        {
            return true;
        }
        if (got < 0)
        {
            return WSAGetLastError() == WSAECONNRESET;
        }
    }
}

void TestLinesAndReplies()
{
    std::printf("lines and replies\n");
    Server       server;
    std::wstring error;
    EXPECT(server.Open(kTestPort, error) && server.IsOpen());
    const SOCKET client = Connect();
    EXPECT(client != INVALID_SOCKET);
    SendText(client, "join\r\nwait-un");
    std::this_thread::sleep_for(50ms);
    SendText(client, "til p2.in == on\n\n");
    const auto events = Collect(server, 3);
    EXPECT(events.size() == 3);
    if (events.size() == 3)
    {
        EXPECT(events[0].line == "join" && !events[0].closed);
        EXPECT(events[1].line == "wait-until p2.in == on");
        EXPECT(events[2].line.empty() && !events[2].closed);  // a blank line is a line
        EXPECT(events[0].client == events[2].client);
        server.Send(events[0].client, "ok 1 join");
        server.Send(events[0].client + 99, "to nobody");  // a client that is gone: dropped quietly
        EXPECT(ReadLine(client) == "ok 1 join");
        EXPECT(ReadLine(client).empty());  // nothing else came (the read times out)
    }
    closesocket(client);
    const auto left = Collect(server, 1);
    EXPECT(left.size() == 1 && left[0].closed && left[0].line.empty());
    server.Close();
    EXPECT(!server.IsOpen());
    EXPECT(Connect() == INVALID_SOCKET);  // nothing listens after Close
    server.Close();                       // a second Close does nothing
}

void TestLongLineCloses()
{
    std::printf("a line over kMaxLine closes its client\n");
    Server       server;
    std::wstring error;
    EXPECT(server.Open(kTestPort, error));
    const SOCKET client = Connect();
    SendText(client, std::string(Server::kMaxLine + 10, 'x'));
    const auto events = Collect(server, 1);
    EXPECT(events.size() == 1 && events[0].closed);
    EXPECT(ClosedByPeer(client));
    closesocket(client);
}

void TestFlushAtClose()
{
    std::printf("Close sends the replies queued before it\n");
    Server       server;
    std::wstring error;
    EXPECT(server.Open(kTestPort, error));
    const SOCKET client = Connect();
    SendText(client, "quit\n");
    const auto events = Collect(server, 1);
    EXPECT(events.size() == 1);
    if (events.size() == 1)
    {
        server.Send(events[0].client, "dropped 1 quit -- the console closed");
        server.Close();  // at once: the reply is still queued
        EXPECT(ReadLine(client) == "dropped 1 quit -- the console closed");
        EXPECT(ClosedByPeer(client));
    }
    closesocket(client);
}

void TestUnreadRepliesClose()
{
    std::printf("a client that leaves kMaxPending bytes unread is closed\n");
    Server       server;
    std::wstring error;
    EXPECT(server.Open(kTestPort, error));
    const SOCKET client = Connect();  // never reads
    SendText(client, "help\n");
    const auto events = Collect(server, 1);
    EXPECT(events.size() == 1);
    if (events.size() == 1)
    {
        // A megabyte of replies per round. Winsock takes a whole send while the socket has any room, so the first
        // rounds vanish into its buffers; once they are full the replies pile up in the server, past kMaxPending.
        const std::string    chunk(64 * 1024, 'r');
        std::vector<Server::Event> closed;
        for (int round = 0; round < 60 && closed.empty(); ++round)
        {
            for (int i = 0; i < 16; ++i)
            {
                server.Send(events[0].client, chunk);
            }
            std::this_thread::sleep_for(50ms);
            closed = server.Drain();
        }
        EXPECT(closed.size() == 1 && closed[0].closed && closed[0].client == events[0].client);
    }
    closesocket(client);
}

void TestClientLimit()
{
    std::printf("the client limit\n");
    Server       server;
    std::wstring error;
    EXPECT(server.Open(kTestPort, error));
    std::vector<SOCKET> clients;
    for (std::size_t i = 0; i < Server::kMaxClients; ++i)
    {
        clients.push_back(Connect());
        SendText(clients.back(), "status\n");
    }
    const auto events = Collect(server, Server::kMaxClients);
    EXPECT(events.size() == Server::kMaxClients);
    const SOCKET extra = Connect();  // the backlog takes it; the server closes it
    EXPECT(extra != INVALID_SOCKET && ClosedByPeer(extra));
    closesocket(extra);
    for (const SOCKET client : clients)
    {
        closesocket(client);
    }
}

void TestPortTaken()
{
    std::printf("a taken port\n");
    Server       first;
    Server       second;
    std::wstring error;
    EXPECT(first.Open(kTestPort, error));
    EXPECT(!second.Open(kTestPort, error) && !second.IsOpen() && !error.empty());
}

}  // namespace

int main()
{
    WSADATA data{};  // the clients' own, so a check after the server's Close still has sockets
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        std::printf("WSAStartup failed\n");
        return 1;
    }
    TestLinesAndReplies();
    TestLongLineCloses();
    TestFlushAtClose();
    TestUnreadRepliesClose();
    TestClientLimit();
    TestPortTaken();
    WSACleanup();
    return lss::test::Report();
}
