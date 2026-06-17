/*
    Copyright 2016-2026 melonDS team / KHWaterMelonMix

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "Relay.h"
#include "Platform.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <chrono>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
#endif

namespace melonDS
{

using Platform::Log;
using Platform::LogLevel;

// ─── helpers ──────────────────────────────────────────────────────────────────

static void SetNonBlocking(socket_t s, bool nb)
{
#ifdef _WIN32
    u_long mode = nb ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (nb) flags |= O_NONBLOCK;
    else    flags &= ~O_NONBLOCK;
    fcntl(s, F_SETFL, flags);
#endif
}

static void SetNoDelay(socket_t s)
{
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
}

// Blocking send of exactly `len` bytes.
static bool SendAll(socket_t s, const u8* data, int len)
{
    int sent = 0;
    while (sent < len)
    {
        int r = send(s, (const char*)(data + sent), len - sent, 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

// Blocking recv of exactly `len` bytes.
static bool RecvAll(socket_t s, u8* data, int len)
{
    int got = 0;
    while (got < len)
    {
        int r = recv(s, (char*)(data + got), len - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

static u64 NowUS()
{
    using namespace std::chrono;
    return (u64)duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count();
}

// ─── GetLocalIPAddress ────────────────────────────────────────────────────────

std::string GetLocalIPAddress()
{
    // Connect a UDP socket to an external address (doesn't actually send)
    // to discover which local interface the OS would use.
    socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return "Unknown";

    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family      = AF_INET;
    remote.sin_port        = htons(7100);
    remote.sin_addr.s_addr = inet_addr("8.8.8.8");

    if (connect(s, (struct sockaddr*)&remote, sizeof(remote)) != 0)
    {
        closesocket(s);
        return "Unknown";
    }

    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    getsockname(s, (struct sockaddr*)&local, &len);
    closesocket(s);

    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
    return std::string(buf);
}

// ─── RelayServer ─────────────────────────────────────────────────────────────

RelayServer::RelayServer()
    : ListenSock(INVALID_SOCKET), Port(kRelayPort), MaxPlayers(4), Running(false)
{
    memset(RoomCode, 0, sizeof(RoomCode));
    memset(HostName, 0, sizeof(HostName));
}

RelayServer::~RelayServer()
{
    Stop();
}

bool RelayServer::Start(const char* roomCode, const char* hostName, int maxPlayers)
{
    if (Running) Stop();

    MaxPlayers = maxPlayers;
    strncpy(RoomCode, roomCode, 6); RoomCode[6] = '\0';
    strncpy(HostName, hostName, 31); HostName[31] = '\0';

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    ListenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ListenSock == INVALID_SOCKET)
    {
        Log(LogLevel::Error, "Relay: socket() failed\n");
        return false;
    }

    int opt = 1;
    setsockopt(ListenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    // Try ports kRelayPort ... kRelayPort+99
    bool bound = false;
    for (int p = kRelayPort; p < kRelayPort + 100; p++)
    {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons((u16)p);

        if (bind(ListenSock, (struct sockaddr*)&addr, sizeof(addr)) == 0)
        {
            Port = p;
            bound = true;
            break;
        }
    }

    if (!bound)
    {
        Log(LogLevel::Error, "Relay: bind() failed on all ports\n");
        closesocket(ListenSock);
        ListenSock = INVALID_SOCKET;
        return false;
    }

    if (listen(ListenSock, 16) != 0)
    {
        Log(LogLevel::Error, "Relay: listen() failed\n");
        closesocket(ListenSock);
        ListenSock = INVALID_SOCKET;
        return false;
    }

    // Add host as player 0
    {
        std::lock_guard<std::mutex> lk(ClientsMutex);
        Clients.clear();
        ClientConn host;
        host.Sock       = INVALID_SOCKET; // host is local, no socket
        host.Handshaked = true;
        host.Ping       = 0;
        memset(&host.Player, 0, sizeof(host.Player));
        host.Player.ID        = 0;
        host.Player.Connected = true;
        host.Player.Address   = 0;
        strncpy(host.Player.Name, HostName, 31);
        Clients.push_back(host);
    }

    Running = true;
    AcceptThread = std::thread(&RelayServer::AcceptLoop, this);

    Log(LogLevel::Info, "Relay: server started on port %d, code %s\n", Port, RoomCode);
    return true;
}

void RelayServer::Stop()
{
    if (!Running) return;
    Running = false;

    if (ListenSock != INVALID_SOCKET)
    {
        closesocket(ListenSock);
        ListenSock = INVALID_SOCKET;
    }

    if (AcceptThread.joinable())
        AcceptThread.join();

    {
        std::lock_guard<std::mutex> lk(ClientsMutex);
        for (auto& c : Clients)
        {
            if (c.Sock != INVALID_SOCKET)
                closesocket(c.Sock);
        }
        Clients.clear();
    }

#ifdef _WIN32
    WSACleanup();
#endif
}

void RelayServer::AcceptLoop()
{
    SetNonBlocking(ListenSock, true);

    while (Running)
    {
        // Use select with a short timeout so we can check Running
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ListenSock, &fds);
        struct timeval tv = {0, 50000}; // 50ms
        int r = select((int)ListenSock + 1, &fds, nullptr, nullptr, &tv);

        if (r > 0 && FD_ISSET(ListenSock, &fds))
        {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            socket_t csock = accept(ListenSock, (struct sockaddr*)&caddr, &clen);
            if (csock == INVALID_SOCKET) continue;

            SetNoDelay(csock);

            std::lock_guard<std::mutex> lk(ClientsMutex);

            // Find a free slot (skip slot 0 = host)
            int slot = -1;
            for (int i = 1; i < MaxPlayers; i++)
            {
                bool used = false;
                for (auto& c : Clients)
                    if (c.Player.ID == i) { used = true; break; }
                if (!used) { slot = i; break; }
            }

            if (slot < 0 || (int)Clients.size() >= MaxPlayers)
            {
                // Full — send NAK and close
                RelayMsgHeader nak;
                nak.Magic  = kRelayMagic;
                nak.Type   = RMsg_HelloNak;
                nak.Length = 0;
                SendAll(csock, (u8*)&nak, sizeof(nak));
                closesocket(csock);
                continue;
            }

            ClientConn cc;
            cc.Sock       = csock;
            cc.Handshaked = false;
            cc.Ping       = 0;
            memset(&cc.Player, 0, sizeof(cc.Player));
            cc.Player.ID      = slot;
            cc.Player.Address = caddr.sin_addr.s_addr;
            Clients.push_back(std::move(cc));

            Log(LogLevel::Info, "Relay: new connection slot %d\n", slot);
        }

        // Service all connected clients (non-blocking reads)
        {
            std::lock_guard<std::mutex> lk(ClientsMutex);
            for (auto& c : Clients)
            {
                if (c.Sock == INVALID_SOCKET) continue;
                ServiceClient(c);
            }

            // Remove disconnected clients
            Clients.erase(
                std::remove_if(Clients.begin(), Clients.end(),
                    [](const ClientConn& c){
                        return c.Sock != INVALID_SOCKET && !c.Player.Connected && c.Handshaked;
                    }),
                Clients.end());
        }
    }
}

bool RelayServer::DoHandshake(ClientConn& c)
{
    // Hello payload: magic(4) version(4) code(6) name(32)
    struct HelloPayload
    {
        u32  Magic;
        u32  Version;
        char Code[6];
        char Name[32];
    };

    SetNonBlocking(c.Sock, false);  // blocking for handshake
    struct timeval tv = {3, 0};
    setsockopt(c.Sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    RelayMsgHeader hdr;
    if (!RecvAll(c.Sock, (u8*)&hdr, sizeof(hdr))) return false;
    if (hdr.Magic != kRelayMagic) return false;
    if (hdr.Type  != RMsg_Hello)  return false;
    if (hdr.Length != sizeof(HelloPayload)) return false;

    HelloPayload payload;
    if (!RecvAll(c.Sock, (u8*)&payload, sizeof(payload))) return false;
    if (payload.Magic   != kRelayMagic)   return false;
    if (payload.Version != kRelayVersion) return false;

    // Validate code
    if (memcmp(payload.Code, RoomCode, 6) != 0)
    {
        RelayMsgHeader nak = {kRelayMagic, RMsg_HelloNak, 0};
        SendAll(c.Sock, (u8*)&nak, sizeof(nak));
        return false;
    }

    // Copy name
    memcpy(c.Player.Name, payload.Name, 32);
    c.Player.Name[31]   = '\0';
    c.Player.Connected  = true;

    // Send ACK: playerID(1) + numPlayers(1)
    struct AckPayload { u8 PlayerID; u8 MaxPlayers; };
    AckPayload ack = { (u8)c.Player.ID, (u8)MaxPlayers };
    RelayMsgHeader ackhdr = {kRelayMagic, RMsg_HelloAck, sizeof(ack)};
    if (!SendAll(c.Sock, (u8*)&ackhdr, sizeof(ackhdr))) return false;
    if (!SendAll(c.Sock, (u8*)&ack, sizeof(ack)))       return false;

    SetNonBlocking(c.Sock, true);  // back to non-blocking for main loop
    tv = {0, 0};
    setsockopt(c.Sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    Log(LogLevel::Info, "Relay: player %d '%s' connected\n", c.Player.ID, c.Player.Name);
    return true;
}

void RelayServer::ServiceClient(ClientConn& c)
{
    if (!c.Handshaked)
    {
        c.Handshaked = true;
        if (!DoHandshake(c))
        {
            Log(LogLevel::Warn, "Relay: handshake failed for slot %d\n", c.Player.ID);
            closesocket(c.Sock);
            c.Sock = INVALID_SOCKET;
            c.Player.Connected = false;
            return;
        }
        BroadcastPlayerList();
        return;
    }

    // Non-blocking read
    u8 hdrbuf[sizeof(RelayMsgHeader)];
    int r = recv(c.Sock, (char*)hdrbuf, sizeof(hdrbuf), MSG_PEEK);
    if (r <= 0) return;  // nothing ready
    if (r < (int)sizeof(RelayMsgHeader)) return;

    // Consume the header
    recv(c.Sock, (char*)hdrbuf, sizeof(hdrbuf), 0);
    RelayMsgHeader* hdr = (RelayMsgHeader*)hdrbuf;
    if (hdr->Magic != kRelayMagic)
    {
        // Corrupt stream — disconnect
        closesocket(c.Sock);
        c.Sock = INVALID_SOCKET;
        c.Player.Connected = false;
        BroadcastPlayerList();
        return;
    }

    u32 len = hdr->Length;
    std::vector<u8> payload(len);
    if (len > 0)
    {
        // May need multiple reads for large packets
        int got = 0;
        while (got < (int)len)
        {
            int n = recv(c.Sock, (char*)payload.data() + got, len - got, 0);
            if (n <= 0) return;
            got += n;
        }
    }

    switch (hdr->Type)
    {
    case RMsg_MPPacket:
    {
        if (len < sizeof(MPPacketHeader)) break;
        MPPacketHeader* mph = (MPPacketHeader*)payload.data();
        mph->SenderID = c.Player.ID;

        u32 mptype = mph->Type & 0xFFFF;

        // Route to host's RX queue (non-broadcast packets to/from host)
        {
            std::lock_guard<std::mutex> lk(RXMutex);
            RXEntry entry;
            entry.Data.assign(payload.begin() + sizeof(MPPacketHeader),
                              payload.end());
            entry.Timestamp = mph->Timestamp;
            entry.Type      = mph->Type;
            entry.Aid       = (u16)(mph->Type >> 16);

            if (mptype == 2)  // reply → host's reply queue
                RXHostQueue.push(entry);
            else
                RXQueue.push(entry);
        }

        // Broadcast to all other clients (not back to sender, not to host
        // because host reads via RXQueue above)
        BroadcastPacket((u8*)hdr, sizeof(RelayMsgHeader),
                        payload.data(), len,
                        c.Player.ID);
        break;
    }

    case RMsg_Disconnect:
        closesocket(c.Sock);
        c.Sock = INVALID_SOCKET;
        c.Player.Connected = false;
        BroadcastPlayerList();
        break;

    default:
        break;
    }
}

void RelayServer::BroadcastPlayerList()
{
    // Build payload: count(1) + RelayPlayer[count]
    std::vector<u8> payload;
    u8 count = 0;
    for (auto& c : Clients)
        if (c.Player.Connected) count++;

    payload.push_back(count);
    for (auto& c : Clients)
    {
        if (!c.Player.Connected) continue;
        payload.resize(payload.size() + sizeof(RelayPlayer));
        memcpy(payload.data() + payload.size() - sizeof(RelayPlayer),
               &c.Player, sizeof(RelayPlayer));
    }

    RelayMsgHeader hdr = {kRelayMagic, RMsg_PlayerList, (u32)payload.size()};
    BroadcastPacket((u8*)&hdr, sizeof(hdr), payload.data(), (int)payload.size());
}

void RelayServer::BroadcastPacket(const u8* header, int headerLen,
                                   const u8* payload, int payloadLen,
                                   int excludeSlot)
{
    for (auto& c : Clients)
    {
        if (c.Sock == INVALID_SOCKET) continue;
        if (c.Player.ID == excludeSlot) continue;
        if (!c.Player.Connected) continue;

        SendAll(c.Sock, header, headerLen);
        if (payloadLen > 0)
            SendAll(c.Sock, payload, payloadLen);
    }
}

void RelayServer::SendToClient(ClientConn& c, const u8* data, int len)
{
    if (c.Sock == INVALID_SOCKET) return;
    SendAll(c.Sock, data, len);
}

void RelayServer::Process()
{
    // Called from the emulator's main thread every video frame.
    // The heavy lifting is done on AcceptThread; this just keeps
    // the interface ticking. We could do a quick non-blocking poll here
    // if we wanted, but it's not needed given AcceptLoop runs continuously.
}

std::vector<RelayPlayer> RelayServer::GetPlayerList()
{
    std::lock_guard<std::mutex> lk(ClientsMutex);
    std::vector<RelayPlayer> list;
    for (auto& c : Clients)
        if (c.Player.Connected)
            list.push_back(c.Player);
    return list;
}

int RelayServer::GetNumPlayers()
{
    std::lock_guard<std::mutex> lk(ClientsMutex);
    int n = 0;
    for (auto& c : Clients)
        if (c.Player.Connected) n++;
    return n;
}

// HOST-SIDE packet injection: the host's Wifi.cpp uses these to send/recv
// as if it were talking to LAN. The relay server delivers the host's
// outgoing packets to all clients directly.

int RelayServer::SendPacket(int inst, u8* data, int len, u64 timestamp)
{
    MPPacketHeader mph;
    mph.Magic     = 0x4946494E;
    mph.SenderID  = 0;
    mph.Type      = 0;
    mph.Length    = len;
    mph.Timestamp = timestamp;

    std::vector<u8> pkt(sizeof(RelayMsgHeader) + sizeof(MPPacketHeader) + len);
    RelayMsgHeader hdr = {kRelayMagic, RMsg_MPPacket,
                          (u32)(sizeof(MPPacketHeader) + len)};
    memcpy(pkt.data(), &hdr, sizeof(hdr));
    memcpy(pkt.data() + sizeof(hdr), &mph, sizeof(mph));
    if (len) memcpy(pkt.data() + sizeof(hdr) + sizeof(mph), data, len);

    std::lock_guard<std::mutex> lk(ClientsMutex);
    BroadcastPacket(pkt.data(), sizeof(RelayMsgHeader),
                    pkt.data() + sizeof(RelayMsgHeader),
                    (int)(sizeof(MPPacketHeader) + len), 0);
    return len;
}

int RelayServer::RecvPacket(int inst, u8* data, u64* timestamp)
{
    return RecvGeneric(RXQueue, data, timestamp, false, 0);
}

int RelayServer::SendCmd(int inst, u8* data, int len, u64 timestamp)
{
    MPPacketHeader mph = {0x4946494E, 0, 1, (u32)len, timestamp};
    RelayMsgHeader hdr = {kRelayMagic, RMsg_MPPacket,
                          (u32)(sizeof(mph) + len)};
    std::lock_guard<std::mutex> lk(ClientsMutex);
    for (auto& c : Clients)
    {
        if (c.Sock == INVALID_SOCKET || !c.Player.Connected) continue;
        SendAll(c.Sock, (u8*)&hdr, sizeof(hdr));
        SendAll(c.Sock, (u8*)&mph, sizeof(mph));
        if (len) SendAll(c.Sock, data, len);
    }
    return len;
}

int RelayServer::SendReply(int inst, u8* data, int len, u64 timestamp, u16 aid)
{
    // Host replies go directly into the local RX reply queue (loopback)
    std::lock_guard<std::mutex> lk(RXMutex);
    RXEntry entry;
    if (data && len > 0)
        entry.Data.assign(data, data + len);
    entry.Timestamp = timestamp;
    entry.Type      = 2 | ((u32)aid << 16);
    entry.Aid       = aid;
    RXHostQueue.push(entry);
    return len;
}

int RelayServer::SendAck(int inst, u8* data, int len, u64 timestamp)
{
    MPPacketHeader mph = {0x4946494E, 0, 3, (u32)len, timestamp};
    RelayMsgHeader hdr = {kRelayMagic, RMsg_MPPacket,
                          (u32)(sizeof(mph) + len)};
    std::lock_guard<std::mutex> lk(ClientsMutex);
    for (auto& c : Clients)
    {
        if (c.Sock == INVALID_SOCKET || !c.Player.Connected) continue;
        SendAll(c.Sock, (u8*)&hdr, sizeof(hdr));
        SendAll(c.Sock, (u8*)&mph, sizeof(mph));
        if (len) SendAll(c.Sock, data, len);
    }
    return len;
}

int RelayServer::RecvHostPacket(int inst, u8* data, u64* timestamp)
{
    // KHWaterMelonMix: non-blocking for same reason as client side.
    return RecvGeneric(RXQueue, data, timestamp, false);
}

u16 RelayServer::RecvReplies(int inst, u8* packets, u64 timestamp, u16 aidmask)
{
    u16 ret = 0;

    // KHWaterMelonMix: wait up to 8ms for client replies.
    // RecvReplies is called once per CMD cycle from ProcessTX, not every
    // timer tick, so an 8ms wait here doesn't hurt framerate but gives
    // the relay enough time to route the client's reply back to the host.
    u64 deadline = NowUS() + 5000ULL;
    while (NowUS() < deadline)
    {
        {
            std::lock_guard<std::mutex> lk(RXMutex);
            if (!RXHostQueue.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    std::lock_guard<std::mutex> lk(RXMutex);
    while (!RXHostQueue.empty())
    {
        RXEntry& e = RXHostQueue.front();
        if ((e.Type & 0xFFFF) == 2)
        {
            u16 aid = e.Aid;
            if (aid > 0 && aid < 16 && (aidmask & (1 << aid)))
            {
                int sz = (int)e.Data.size();
                if (sz > 1024) sz = 1024;
                memcpy(&packets[(aid-1)*1024], e.Data.data(), sz);
                ret |= (1 << aid);
            }
        }
        RXHostQueue.pop();
    }
    return ret;
}

int RelayServer::RecvGeneric(std::queue<RXEntry>& q, u8* data, u64* timestamp,
                              bool block, u32 typeFilter, int timeoutMS)
{
    if (block)
    {
        u64 deadline = NowUS() + (u64)timeoutMS * 1000ULL;
        while (NowUS() < deadline)
        {
            {
                std::lock_guard<std::mutex> lk(RXMutex);
                if (!q.empty())
                {
                    RXEntry& e = q.front();
                    if (typeFilter == 0xFFFFFFFF || (e.Type & 0xFFFF) == typeFilter)
                    {
                        int sz = (int)e.Data.size();
                        memcpy(data, e.Data.data(), sz);
                        if (timestamp) *timestamp = e.Timestamp;
                        q.pop();
                        return sz;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 0;
    }
    else
    {
        std::lock_guard<std::mutex> lk(RXMutex);
        if (q.empty()) return 0;
        RXEntry& e = q.front();
        int sz = (int)e.Data.size();
        if (sz > 2048) sz = 2048;
        memcpy(data, e.Data.data(), sz);
        if (timestamp) *timestamp = e.Timestamp;
        q.pop();
        return sz;
    }
}

// ─── RelayClient ─────────────────────────────────────────────────────────────

RelayClient::RelayClient()
    : Sock(INVALID_SOCKET), Connected(false), MaxPlayers(4), MyPlayerID(-1)
{
}

RelayClient::~RelayClient()
{
    Disconnect();
}

bool RelayClient::Connect(const char* hostIP, int port,
                          const char* roomCode, const char* playerName)
{
    if (Connected) Disconnect();

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    Sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Sock == INVALID_SOCKET) return false;

    SetNoDelay(Sock);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u16)port);
    if (inet_pton(AF_INET, hostIP, &addr.sin_addr) != 1)
    {
        closesocket(Sock);
        Sock = INVALID_SOCKET;
        return false;
    }

    // 5-second connect timeout
    SetNonBlocking(Sock, true);
    connect(Sock, (struct sockaddr*)&addr, sizeof(addr));

    fd_set fds;
    FD_ZERO(&fds); FD_SET(Sock, &fds);
    struct timeval tv = {5, 0};
    int r = select((int)Sock + 1, nullptr, &fds, nullptr, &tv);
    if (r <= 0)
    {
        closesocket(Sock);
        Sock = INVALID_SOCKET;
        return false;
    }
    SetNonBlocking(Sock, false);

    // Send Hello
    struct HelloPayload
    {
        u32  Magic;
        u32  Version;
        char Code[6];
        char Name[32];
    };

    HelloPayload hello;
    hello.Magic   = kRelayMagic;
    hello.Version = kRelayVersion;
    memset(hello.Code, 0, 6);
    memcpy(hello.Code, roomCode, std::min((int)strlen(roomCode), 6));
    memset(hello.Name, 0, 32);
    strncpy(hello.Name, playerName, 31);

    RelayMsgHeader hdr = {kRelayMagic, RMsg_Hello, sizeof(hello)};
    if (!SendAll(Sock, (u8*)&hdr, sizeof(hdr)) ||
        !SendAll(Sock, (u8*)&hello, sizeof(hello)))
    {
        closesocket(Sock); Sock = INVALID_SOCKET;
        return false;
    }

    // Receive ACK or NAK
    struct timeval rtv = {5, 0};
    setsockopt(Sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rtv, sizeof(rtv));

    RelayMsgHeader ackhdr;
    if (!RecvAll(Sock, (u8*)&ackhdr, sizeof(ackhdr)))
    {
        closesocket(Sock); Sock = INVALID_SOCKET;
        return false;
    }

    if (ackhdr.Magic != kRelayMagic || ackhdr.Type == RMsg_HelloNak)
    {
        closesocket(Sock); Sock = INVALID_SOCKET;
        Log(LogLevel::Warn, "Relay: server rejected connection (bad code or full)\n");
        return false;
    }

    if (ackhdr.Type != RMsg_HelloAck)
    {
        closesocket(Sock); Sock = INVALID_SOCKET;
        return false;
    }

    struct AckPayload { u8 PlayerID; u8 MaxPlayers; };
    AckPayload ackpay;
    if (!RecvAll(Sock, (u8*)&ackpay, sizeof(ackpay)))
    {
        closesocket(Sock); Sock = INVALID_SOCKET;
        return false;
    }

    MyPlayerID = ackpay.PlayerID;
    MaxPlayers = ackpay.MaxPlayers;

    // Remove receive timeout and start recv thread
    struct timeval ztv = {0, 0};
    setsockopt(Sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ztv, sizeof(ztv));
    SetNonBlocking(Sock, false);

    Connected = true;
    RecvThread = std::thread(&RelayClient::RecvLoop, this);

    Log(LogLevel::Info, "Relay: connected as player %d\n", MyPlayerID);
    return true;
}

void RelayClient::Disconnect()
{
    if (!Connected && Sock == INVALID_SOCKET) return;

    Connected = false;

    if (Sock != INVALID_SOCKET)
    {
        // Send disconnect notice
        RelayMsgHeader hdr = {kRelayMagic, RMsg_Disconnect, 0};
        SendAll(Sock, (u8*)&hdr, sizeof(hdr));
        closesocket(Sock);
        Sock = INVALID_SOCKET;
    }

    if (RecvThread.joinable())
        RecvThread.join();

#ifdef _WIN32
    WSACleanup();
#endif
}

void RelayClient::RecvLoop()
{
    while (Connected && Sock != INVALID_SOCKET)
    {
        RelayMsgHeader hdr;
        if (!RecvAll(Sock, (u8*)&hdr, sizeof(hdr)))
        {
            Connected = false;
            break;
        }

        if (hdr.Magic != kRelayMagic)
        {
            Connected = false;
            break;
        }

        std::vector<u8> payload(hdr.Length);
        if (hdr.Length > 0 && !RecvAll(Sock, payload.data(), hdr.Length))
        {
            Connected = false;
            break;
        }

        switch (hdr.Type)
        {
        case RMsg_PlayerList:
        {
            if (payload.empty()) break;
            u8 count = payload[0];
            std::lock_guard<std::mutex> lk(PlayersMutex);
            Players.clear();
            for (int i = 0; i < count; i++)
            {
                int off = 1 + i * sizeof(RelayPlayer);
                if (off + (int)sizeof(RelayPlayer) > (int)payload.size()) break;
                RelayPlayer p;
                memcpy(&p, payload.data() + off, sizeof(RelayPlayer));
                Players.push_back(p);
            }
            break;
        }

        case RMsg_MPPacket:
        {
            if (hdr.Length < sizeof(MPPacketHeader)) break;
            MPPacketHeader* mph = (MPPacketHeader*)payload.data();
            u32 mptype = mph->Type & 0xFFFF;

            std::lock_guard<std::mutex> lk(RXMutex);
            RXEntry entry;
            entry.Data.assign(payload.begin() + sizeof(MPPacketHeader),
                              payload.end());
            entry.Timestamp = mph->Timestamp;
            entry.Type      = mph->Type;
            entry.Aid       = (u16)(mph->Type >> 16);

            if (mptype == 1 || mptype == 3)  // CMD or ACK → host queue
                RXHostQueue.push(entry);
            else
                RXQueue.push(entry);  // type 0 (beacons/general) and type 2 (replies) → RXQueue
            break;
        }

        case RMsg_Disconnect:
            Connected = false;
            break;

        default:
            break;
        }
    }
}

void RelayClient::Process()
{
    // Recv thread handles everything; nothing needed here.
}

std::vector<RelayPlayer> RelayClient::GetPlayerList()
{
    std::lock_guard<std::mutex> lk(PlayersMutex);
    return Players;
}

int RelayClient::SendPacket(int inst, u8* data, int len, u64 timestamp)
{
    MPPacketHeader mph = {0x4946494E, (u32)MyPlayerID, 0, (u32)len, timestamp};
    RelayMsgHeader hdr = {kRelayMagic, RMsg_MPPacket,
                          (u32)(sizeof(mph) + len)};
    if (!SendAll(Sock, (u8*)&hdr, sizeof(hdr))) return 0;
    if (!SendAll(Sock, (u8*)&mph, sizeof(mph))) return 0;
    if (len && !SendAll(Sock, data, len))        return 0;
    return len;
}

int RelayClient::RecvPacket(int inst, u8* data, u64* timestamp)
{
    return RecvGeneric(RXQueue, data, timestamp, false);
}

int RelayClient::SendCmd(int inst, u8* data, int len, u64 timestamp)
{
    MPPacketHeader mph = {0x4946494E, (u32)MyPlayerID, 1, (u32)len, timestamp};
    RelayMsgHeader hdr = {kRelayMagic, RMsg_MPPacket,
                          (u32)(sizeof(mph) + len)};
    if (!SendAll(Sock, (u8*)&hdr, sizeof(hdr))) return 0;
    if (!SendAll(Sock, (u8*)&mph, sizeof(mph))) return 0;
    if (len && !SendAll(Sock, data, len))        return 0;
    return len;
}

int RelayClient::SendReply(int inst, u8* data, int len, u64 timestamp, u16 aid)
{
    u32 type = 2 | ((u32)aid << 16);
    MPPacketHeader mph = {0x4946494E, (u32)MyPlayerID, type, (u32)len, timestamp};
    RelayMsgHeader hdr = {kRelayMagic, RMsg_MPPacket,
                          (u32)(sizeof(mph) + len)};
    if (!SendAll(Sock, (u8*)&hdr, sizeof(hdr))) return 0;
    if (!SendAll(Sock, (u8*)&mph, sizeof(mph))) return 0;
    if (len && !SendAll(Sock, data, len))        return 0;
    return len;
}

int RelayClient::SendAck(int inst, u8* data, int len, u64 timestamp)
{
    MPPacketHeader mph = {0x4946494E, (u32)MyPlayerID, 3, (u32)len, timestamp};
    RelayMsgHeader hdr = {kRelayMagic, RMsg_MPPacket,
                          (u32)(sizeof(mph) + len)};
    if (!SendAll(Sock, (u8*)&hdr, sizeof(hdr))) return 0;
    if (!SendAll(Sock, (u8*)&mph, sizeof(mph))) return 0;
    if (len && !SendAll(Sock, data, len))        return 0;
    return len;
}

int RelayClient::RecvHostPacket(int inst, u8* data, u64* timestamp)
{
    // KHWaterMelonMix: must be non-blocking. USTimer calls this up to
    // 2083 times per frame (every 8us). Any blocking multiplies directly
    // into frame time. The recv thread populates RXHostQueue asynchronously.
    return RecvGeneric(RXHostQueue, data, timestamp, false);
}


u16 RelayClient::RecvReplies(int inst, u8* packets, u64 timestamp, u16 aidmask)
{
    u16 ret = 0;
    std::lock_guard<std::mutex> lk(RXMutex);
    while (!RXHostQueue.empty())
    {
        RXEntry& e = RXHostQueue.front();
        if ((e.Type & 0xFFFF) == 2)
        {
            u16 aid = e.Aid;
            if (aid > 0 && aid < 16 && (aidmask & (1 << aid)))
            {
                int sz = (int)e.Data.size();
                if (sz > 1024) sz = 1024;
                memcpy(&packets[(aid-1)*1024], e.Data.data(), sz);
                ret |= (1 << aid);
            }
        }
        RXHostQueue.pop();
    }
    return ret;
}

int RelayClient::RecvGeneric(std::queue<RXEntry>& q, u8* data, u64* timestamp,
                              bool block, u32 typeFilter, int timeoutMS)
{
    if (block)
    {
        u64 deadline = NowUS() + (u64)timeoutMS * 1000ULL;
        while (NowUS() < deadline)
        {
            {
                std::lock_guard<std::mutex> lk(RXMutex);
                if (!q.empty())
                {
                    RXEntry& e = q.front();
                    int sz = (int)e.Data.size();
                    if (sz > 2048) sz = 2048;
                    memcpy(data, e.Data.data(), sz);
                    if (timestamp) *timestamp = e.Timestamp;
                    q.pop();
                    return sz;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 0;
    }
    else
    {
        std::lock_guard<std::mutex> lk(RXMutex);
        if (q.empty()) return 0;
        RXEntry& e = q.front();
        int sz = (int)e.Data.size();
        if (sz > 2048) sz = 2048;
        memcpy(data, e.Data.data(), sz);
        if (timestamp) *timestamp = e.Timestamp;
        q.pop();
        return sz;
    }
}

// ─── Relay MPInterface ────────────────────────────────────────────────────────

Relay::Relay() noexcept
    : Server(nullptr), Client(nullptr)
{
    memset(RoomCode, 0, sizeof(RoomCode));
}

Relay::~Relay() noexcept
{
    EndSession();
}

std::string Relay::GenerateCode()
{
    // 6 random decimal digits, seeded from time
    static bool seeded = false;
    if (!seeded) { srand((unsigned)time(nullptr)); seeded = true; }
    char buf[7];
    snprintf(buf, sizeof(buf), "%06d", rand() % 1000000);
    return std::string(buf);
}

bool Relay::HostGame(const char* playerName, int maxPlayers)
{
    EndSession();
    std::string code = GenerateCode();
    strncpy(RoomCode, code.c_str(), 6);
    RoomCode[6] = '\0';

    Server = new RelayServer();
    if (!Server->Start(RoomCode, playerName, maxPlayers))
    {
        delete Server;
        Server = nullptr;
        return false;
    }
    return true;
}

bool Relay::JoinGame(const char* playerName, const char* hostIP,
                     const char* roomCode)
{
    EndSession();
    strncpy(RoomCode, roomCode, 6);
    RoomCode[6] = '\0';

    Client = new RelayClient();
    if (!Client->Connect(hostIP, kRelayPort, roomCode, playerName))
    {
        delete Client;
        Client = nullptr;
        return false;
    }
    return true;
}

void Relay::EndSession()
{
    if (Server) { Server->Stop(); delete Server; Server = nullptr; }
    if (Client) { Client->Disconnect(); delete Client; Client = nullptr; }
    memset(RoomCode, 0, sizeof(RoomCode));
}

bool Relay::IsConnected() const
{
    if (Server) return Server->IsRunning();
    if (Client) return Client->IsConnected();
    return false;
}

int Relay::GetPort() const
{
    if (Server) return Server->GetPort();
    return kRelayPort;
}

std::vector<RelayPlayer> Relay::GetPlayerList()
{
    if (Server) return Server->GetPlayerList();
    if (Client) return Client->GetPlayerList();
    return {};
}

int Relay::GetNumPlayers()
{
    if (Server) return Server->GetNumPlayers();
    if (Client) return Client->GetNumPlayers();
    return 0;
}

int Relay::GetMaxPlayers()
{
    if (Server) return Server->GetMaxPlayers();
    if (Client) return Client->GetMaxPlayers();
    return 0;
}

void Relay::Process()
{
    if (Server) Server->Process();
    if (Client) Client->Process();
}

void Relay::Begin(int inst) { /* nothing needed */ }
void Relay::End(int inst)   { /* nothing needed */ }

int  Relay::SendPacket(int i, u8* d, int l, u64 t)
{ if (Server) return Server->SendPacket(i,d,l,t); if (Client) return Client->SendPacket(i,d,l,t); return 0; }

int  Relay::RecvPacket(int i, u8* d, u64* t)
{ if (Server) return Server->RecvPacket(i,d,t); if (Client) return Client->RecvPacket(i,d,t); return 0; }

int  Relay::SendCmd(int i, u8* d, int l, u64 t)
{ if (Server) return Server->SendCmd(i,d,l,t); if (Client) return Client->SendCmd(i,d,l,t); return 0; }

int  Relay::SendReply(int i, u8* d, int l, u64 t, u16 aid)
{ if (Server) return Server->SendReply(i,d,l,t,aid); if (Client) return Client->SendReply(i,d,l,t,aid); return 0; }

int  Relay::SendAck(int i, u8* d, int l, u64 t)
{ if (Server) return Server->SendAck(i,d,l,t); if (Client) return Client->SendAck(i,d,l,t); return 0; }

int  Relay::RecvHostPacket(int i, u8* d, u64* t)
{ if (Server) return Server->RecvHostPacket(i,d,t); if (Client) return Client->RecvHostPacket(i,d,t); return 0; }

u16  Relay::RecvReplies(int i, u8* d, u64 t, u16 m)
{ if (Server) return Server->RecvReplies(i,d,t,m); if (Client) return Client->RecvReplies(i,d,t,m); return 0; }

} // namespace melonDS
