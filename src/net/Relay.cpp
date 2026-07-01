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

// ─── Module-level state ────────────────────────────────────────────────────
bool RelayModeActive = false;

// ─── Timing helper ─────────────────────────────────────────────────────────
static u64 NowUS()
{
    using namespace std::chrono;
    return (u64)duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count();
}

// ─── Socket helpers ────────────────────────────────────────────────────────
static void SetNoDelay(socket_t s)
{
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
               (const char*)&one, sizeof(one));
}

static void SetNonBlocking(socket_t s, bool nb)
{
#ifdef _WIN32
    u_long mode = nb ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return;
    if (nb) flags |= O_NONBLOCK;
    else    flags &= ~O_NONBLOCK;
    fcntl(s, F_SETFL, flags);
#endif
}

// Send exactly `len` bytes; returns false on error.
static bool SendExact(socket_t s, const u8* data, int len)
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

// Blocking recv of exactly `len` bytes; returns false on error/disconnect.
static bool RecvExact(socket_t s, u8* data, int len)
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

// Build a RelayMsgHeader + optional payload into a single vector.
static std::vector<u8> BuildMsg(RelayMsgType type,
                                 const u8* payload = nullptr,
                                 u32 paylen = 0)
{
    std::vector<u8> buf(sizeof(RelayMsgHeader) + paylen);
    RelayMsgHeader* hdr = (RelayMsgHeader*)buf.data();
    hdr->Magic  = kRelayMagic;
    hdr->Type   = (u32)type;
    hdr->Length = paylen;
    if (payload && paylen)
        memcpy(buf.data() + sizeof(RelayMsgHeader), payload, paylen);
    return buf;
}

// ─── GetLocalIPAddress ─────────────────────────────────────────────────────
std::string GetLocalIPAddress()
{
    socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == (socket_t)INVALID_SOCKET) return "Unknown";

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

// ═══════════════════════════════════════════════════════════════════════════
// RelayServer
// ═══════════════════════════════════════════════════════════════════════════

RelayServer::RelayServer()
    : ListenSock((socket_t)INVALID_SOCKET)
    , Port(kRelayPort)
    , MaxPlayers(4)
    , HostChannel(0)
    , Running(false)
{
    memset(RoomCode, 0, sizeof(RoomCode));
    memset(HostName, 0, sizeof(HostName));
}

RelayServer::~RelayServer()
{
    Stop();
}

bool RelayServer::Start(const char* roomCode, const char* hostName,
                        int maxPlayers, u8 hostChannel)
{
    if (Running) Stop();

    MaxPlayers  = maxPlayers;
    HostChannel = hostChannel;
    strncpy(RoomCode, roomCode,  6); RoomCode[6]  = '\0';
    strncpy(HostName, hostName, 31); HostName[31] = '\0';

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    ListenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ListenSock == (socket_t)INVALID_SOCKET)
    {
        Log(LogLevel::Error, "Relay: socket() failed\n");
        return false;
    }

    int opt = 1;
    setsockopt(ListenSock, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&opt, sizeof(opt));
    SetNoDelay(ListenSock);

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
            Port = p; bound = true; break;
        }
    }

    if (!bound)
    {
        Log(LogLevel::Error, "Relay: bind() failed on all ports\n");
        closesocket(ListenSock);
        ListenSock = (socket_t)INVALID_SOCKET;
        return false;
    }

    if (listen(ListenSock, 16) != 0)
    {
        Log(LogLevel::Error, "Relay: listen() failed\n");
        closesocket(ListenSock);
        ListenSock = (socket_t)INVALID_SOCKET;
        return false;
    }

    // Add host as AID-0 slot (local, no socket)
    {
        std::lock_guard<std::mutex> lk(ClientsMutex);
        Clients.clear();
        ClientConn host;
        host.Peer.AID       = 0;
        host.Peer.Connected = true;
        host.Peer.Address   = 0;
        host.Peer.LastRecvUS = NowUS();
        host.Handshaked     = true;
        strncpy(host.Peer.Name, HostName, 31);
        Clients.push_back(std::move(host));
    }

    Running = true;
    AcceptThread = std::thread(&RelayServer::AcceptLoop, this);

    Log(LogLevel::Info, "Relay: server started port=%d code=%s ch=%d\n",
        Port, RoomCode, HostChannel);
    return true;
}

void RelayServer::Stop()
{
    if (!Running) return;
    Running = false;

    if (ListenSock != (socket_t)INVALID_SOCKET)
    {
        closesocket(ListenSock);
        ListenSock = (socket_t)INVALID_SOCKET;
    }

    if (AcceptThread.joinable())
        AcceptThread.join();

    {
        std::lock_guard<std::mutex> lk(ClientsMutex);
        for (auto& c : Clients)
            if (c.Sock != (socket_t)INVALID_SOCKET)
                closesocket(c.Sock);
        Clients.clear();
    }

#ifdef _WIN32
    WSACleanup();
#endif
}

// ── AcceptLoop ──────────────────────────────────────────────────────────────
// Single-threaded select() loop, exactly like PPSSPP's server_loop.
// 1ms timeout keeps CPU near-idle while still reacting within 1ms.

void RelayServer::AcceptLoop()
{
    SetNonBlocking(ListenSock, true);

    while (Running)
    {
        // Build fd_set from listen socket + all active client sockets
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ListenSock, &fds);
        socket_t maxfd = ListenSock;

        {
            std::lock_guard<std::mutex> lk(ClientsMutex);
            for (auto& c : Clients)
            {
                if (c.Sock == (socket_t)INVALID_SOCKET) continue;
                FD_SET(c.Sock, &fds);
                if (c.Sock > maxfd) maxfd = c.Sock;
            }
        }

        struct timeval tv = {0, 1000}; // 1ms
        int r = select((int)maxfd + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0) continue;

        // ── Accept new connection ─────────────────────────────────────
        if (FD_ISSET(ListenSock, &fds))
        {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            socket_t csock = accept(ListenSock,
                                    (struct sockaddr*)&caddr, &clen);
            if (csock != (socket_t)INVALID_SOCKET)
            {
                SetNoDelay(csock);

                std::lock_guard<std::mutex> lk(ClientsMutex);

                // Find a free AID slot (1..MaxPlayers-1)
                int slot = -1;
                for (int i = 1; i < MaxPlayers; i++)
                {
                    bool used = false;
                    for (auto& c : Clients)
                        if (c.Peer.AID == i) { used = true; break; }
                    if (!used) { slot = i; break; }
                }

                if (slot < 0 || (int)Clients.size() >= MaxPlayers)
                {
                    // Session full — NAK and close
                    auto msg = BuildMsg(RMsg_HelloNak);
                    SendExact(csock, msg.data(), (int)msg.size());
                    closesocket(csock);
                    Log(LogLevel::Info, "Relay: session full, rejected\n");
                }
                else
                {
                    ClientConn cc;
                    cc.Sock             = csock;
                    cc.Handshaked       = false;
                    cc.Peer.AID         = slot;
                    cc.Peer.Connected   = false;
                    cc.Peer.Address     = caddr.sin_addr.s_addr;
                    cc.Peer.LastRecvUS  = NowUS();
                    Clients.push_back(std::move(cc));
                    Log(LogLevel::Info, "Relay: new connection AID=%d\n", slot);
                }
            }
        }

        // ── Service ready clients ─────────────────────────────────────
        {
            std::lock_guard<std::mutex> lk(ClientsMutex);
            for (auto& c : Clients)
            {
                if (c.Sock == (socket_t)INVALID_SOCKET) continue;
                if (!FD_ISSET(c.Sock, &fds)) continue;
                ServiceClient(c);
            }

            // Prune dead connections
            Clients.erase(
                std::remove_if(Clients.begin(), Clients.end(),
                    [](const ClientConn& c)
                    {
                        return c.Sock != (socket_t)INVALID_SOCKET
                            && c.Handshaked
                            && !c.Peer.Connected;
                    }),
                Clients.end());
        }
    }
}

// ── DoHandshake ────────────────────────────────────────────────────────────
// Called once per new connection when the socket first has data ready.
// Runs on AcceptThread with the ClientsMutex HELD — keep it fast.
// Switch socket to blocking for the duration (< 3s timeout).

bool RelayServer::DoHandshake(ClientConn& c)
{
    SetNonBlocking(c.Sock, false);

    struct timeval tv = {3, 0};
    setsockopt(c.Sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    // Read header
    RelayMsgHeader hdr;
    if (!RecvExact(c.Sock, (u8*)&hdr, sizeof(hdr)))
    {
        Log(LogLevel::Error, "Relay: handshake failed at header recv\n");
        return false;
    }
    Log(LogLevel::Info, "Relay: got header magic=%08X type=%u len=%u (expected magic=%08X type=%u len=%zu)\n",
        hdr.Magic, hdr.Type, hdr.Length, kRelayMagic, (u32)RMsg_Hello, sizeof(RelayHelloPayload));

    if (hdr.Magic  != kRelayMagic)
    {
        Log(LogLevel::Error, "Relay: bad magic\n");
        return false;
    }
    if (hdr.Type   != (u32)RMsg_Hello)
    {
        Log(LogLevel::Error, "Relay: bad type\n");
        return false;
    }
    if (hdr.Length != sizeof(RelayHelloPayload))
    {
        Log(LogLevel::Error, "Relay: bad length\n");
        return false;
    }

    // Read payload
    RelayHelloPayload hello;
    if (!RecvExact(c.Sock, (u8*)&hello, sizeof(hello)))      return false;
    if (hello.Magic   != kRelayMagic)                        return false;
    if (hello.Version != kRelayVersion)
    {
        Log(LogLevel::Warn,
            "Relay: client version %u != server version %u\n",
            hello.Version, kRelayVersion);
        // Still accept — we might be backward-compatible
    }

    // Validate room code
    char recvCodeStr[7] = {0};
    memcpy(recvCodeStr, hello.Code, 6);
    Log(LogLevel::Info, "Relay: comparing received code '%s' against server code '%s'\n",
        recvCodeStr, RoomCode);

    if (memcmp(hello.Code, RoomCode, 6) != 0)
    {
        auto msg = BuildMsg(RMsg_HelloNak);
        SendExact(c.Sock, msg.data(), (int)msg.size());
        Log(LogLevel::Warn, "Relay: bad room code from AID=%d\n", c.Peer.AID);
        return false;
    }

    // Copy player name
    memcpy(c.Peer.Name, hello.Name, 32);
    c.Peer.Name[31] = '\0';
    c.Peer.Connected = true;
    c.Peer.LastRecvUS = NowUS();

    // Send ACK
    RelayHelloAckPayload ack;
    ack.PlayerID    = (u8)c.Peer.AID;
    ack.MaxPlayers  = (u8)MaxPlayers;
    ack.HostChannel = HostChannel;
    ack._pad        = 0;

    auto msg = BuildMsg(RMsg_HelloAck, (u8*)&ack, sizeof(ack));
    if (!SendExact(c.Sock, msg.data(), (int)msg.size())) return false;

    // Back to non-blocking for the data loop
    tv = {0, 0};
    setsockopt(c.Sock, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&tv, sizeof(tv));
    SetNonBlocking(c.Sock, true);

    Log(LogLevel::Info, "Relay: handshake OK AID=%d name='%s'\n",
        c.Peer.AID, c.Peer.Name);

    // Notify all players of updated roster
    BroadcastPlayerList();
    return true;
}

// ── ServiceClient ──────────────────────────────────────────────────────────
// Called from AcceptLoop when select() says this socket has data.

void RelayServer::ServiceClient(ClientConn& c)
{
    if (!c.Handshaked)
    {
        c.Handshaked = true; // set before DoHandshake so we don't re-enter
        if (!DoHandshake(c))
        {
            closesocket(c.Sock);
            c.Sock = (socket_t)INVALID_SOCKET;
            c.Peer.Connected = false;
        }
        return;
    }

    // Read header with MSG_PEEK first so we don't block if only partial
    u8 hdrbuf[sizeof(RelayMsgHeader)];
    int r = recv(c.Sock, (char*)hdrbuf, sizeof(hdrbuf), MSG_PEEK);
    if (r < (int)sizeof(RelayMsgHeader))
    {
        if (r == 0 || (r < 0
#ifdef _WIN32
            && WSAGetLastError() != WSAEWOULDBLOCK
#else
            && errno != EAGAIN && errno != EWOULDBLOCK
#endif
            ))
        {
            // Connection closed or real error
            closesocket(c.Sock);
            c.Sock = (socket_t)INVALID_SOCKET;
            c.Peer.Connected = false;
            BroadcastPlayerList();
        }
        return;
    }

    // Consume header
    recv(c.Sock, (char*)hdrbuf, sizeof(hdrbuf), 0);
    RelayMsgHeader* hdr = (RelayMsgHeader*)hdrbuf;

    if (hdr->Magic != kRelayMagic)
    {
        Log(LogLevel::Warn, "Relay: corrupt magic from AID=%d, dropping\n",
            c.Peer.AID);
        closesocket(c.Sock);
        c.Sock = (socket_t)INVALID_SOCKET;
        c.Peer.Connected = false;
        BroadcastPlayerList();
        return;
    }

    // Read payload
    u32 plen = hdr->Length;
    if (plen > 4096)
    {
        Log(LogLevel::Warn, "Relay: oversized packet %u from AID=%d\n",
            plen, c.Peer.AID);
        closesocket(c.Sock);
        c.Sock = (socket_t)INVALID_SOCKET;
        c.Peer.Connected = false;
        BroadcastPlayerList();
        return;
    }

    std::vector<u8> payload(plen);
    if (plen > 0)
    {
        // Use blocking recv for payload (socket was set non-blocking,
        // but we already know data is available from select())
        SetNonBlocking(c.Sock, false);
        bool ok = RecvExact(c.Sock, payload.data(), (int)plen);
        SetNonBlocking(c.Sock, true);
        if (!ok)
        {
            closesocket(c.Sock);
            c.Sock = (socket_t)INVALID_SOCKET;
            c.Peer.Connected = false;
            BroadcastPlayerList();
            return;
        }
    }

    c.Peer.LastRecvUS = NowUS();

    switch ((RelayMsgType)hdr->Type)
    {
    case RMsg_MPPacket:
        DispatchMPPacket(c, payload.data(), plen);
        break;

    case RMsg_Disconnect:
        closesocket(c.Sock);
        c.Sock = (socket_t)INVALID_SOCKET;
        c.Peer.Connected = false;
        BroadcastPlayerList();
        Log(LogLevel::Info, "Relay: AID=%d disconnected\n", c.Peer.AID);
        break;

    default:
        Log(LogLevel::Debug, "Relay: unknown msg type %u from AID=%d\n",
            hdr->Type, c.Peer.AID);
        break;
    }
}

// ── DispatchMPPacket ────────────────────────────────────────────────────────
// Route a DS MP frame from a client to its destination(s).
// Called from AcceptThread with ClientsMutex HELD.

void RelayServer::DispatchMPPacket(ClientConn& sender,
                                    const u8* payload, u32 len)
{
    if (len < sizeof(MPPacketHeader))
    {
        Log(LogLevel::Warn, "Relay: short MPPacket from AID=%d (%u bytes)\n",
            sender.Peer.AID, len);
        return;
    }

    // Stamp the sender's AID into the header (so host knows who replied)
    MPPacketHeader mph;
    memcpy(&mph, payload, sizeof(mph));
    mph.SenderID = (u32)sender.Peer.AID;

    const u8* frameData    = payload + sizeof(MPPacketHeader);
    u32       frameDataLen = (len > sizeof(MPPacketHeader))
                             ? (len - sizeof(MPPacketHeader)) : 0;

    u32 mptype = mph.Type & 0xFFFF;

    // ── Route to host's RX queues ─────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(RXMutex);

        RelayRXEntry entry;
        entry.Data.assign(frameData, frameData + frameDataLen);
        entry.Timestamp  = mph.Timestamp;
        entry.Type       = mph.Type;
        entry.Aid        = (u16)(mph.Type >> 16);
        entry.SenderAID  = sender.Peer.AID;

        if (mptype == 2)
        {
            // Type 2 = client reply → host's reply queue (time-critical)
            RXHostQueue.push(std::move(entry));
        }
        else
        {
            // Type 0/1/3 = general/CMD/ACK → host's general queue
            RXQueue.push(std::move(entry));
        }
    }

    // ── Broadcast to all other clients ────────────────────────────────────
    // Rebuild the packet with the updated header (SenderID stamped)
    std::vector<u8> fwd(sizeof(RelayMsgHeader) + sizeof(MPPacketHeader) + frameDataLen);
    {
        RelayMsgHeader fwdhdr;
        fwdhdr.Magic  = kRelayMagic;
        fwdhdr.Type   = (u32)RMsg_MPPacket;
        fwdhdr.Length = (u32)(sizeof(MPPacketHeader) + frameDataLen);
        memcpy(fwd.data(), &fwdhdr, sizeof(fwdhdr));
        memcpy(fwd.data() + sizeof(fwdhdr), &mph, sizeof(mph));
        if (frameDataLen)
            memcpy(fwd.data() + sizeof(fwdhdr) + sizeof(mph),
                   frameData, frameDataLen);
    }

    BroadcastRaw(fwd.data(), (int)fwd.size(), sender.Peer.AID);
}

// ── BroadcastPlayerList ────────────────────────────────────────────────────
void RelayServer::BroadcastPlayerList()
{
    // Payload: u8 count, then count × RelayPeer structs
    std::vector<u8> payload;
    u8 count = 0;
    for (auto& c : Clients)
        if (c.Peer.Connected) count++;

    payload.push_back(count);
    for (auto& c : Clients)
    {
        if (!c.Peer.Connected) continue;
        size_t off = payload.size();
        payload.resize(off + sizeof(RelayPeer));
        memcpy(payload.data() + off, &c.Peer, sizeof(RelayPeer));
    }

    auto msg = BuildMsg(RMsg_PlayerList, payload.data(), (u32)payload.size());
    BroadcastRaw(msg.data(), (int)msg.size());
}

// ── BroadcastRaw ───────────────────────────────────────────────────────────
// Send raw bytes to all connected clients except excludeAID.
// Caller must hold ClientsMutex.

void RelayServer::BroadcastRaw(const u8* data, int len, int excludeAID)
{
    for (auto& c : Clients)
    {
        if (c.Sock == (socket_t)INVALID_SOCKET) continue;
        if (!c.Peer.Connected) continue;
        if (c.Peer.AID == excludeAID) continue;
        SendExact(c.Sock, data, len);
    }
}

bool RelayServer::SendRaw(socket_t s, const u8* data, int len)
{
    return SendExact(s, data, len);
}

// ── Process ────────────────────────────────────────────────────────────────
void RelayServer::Process()
{
    // AcceptThread does all the work; nothing needed here.
}

// ── GetPeerList / GetNumPlayers ────────────────────────────────────────────
std::vector<RelayPeer> RelayServer::GetPeerList()
{
    std::lock_guard<std::mutex> lk(ClientsMutex);
    std::vector<RelayPeer> list;
    for (auto& c : Clients)
        if (c.Peer.Connected)
            list.push_back(c.Peer);
    return list;
}

int RelayServer::GetNumPlayers()
{
    std::lock_guard<std::mutex> lk(ClientsMutex);
    int n = 0;
    for (auto& c : Clients)
        if (c.Peer.Connected) n++;
    return n;
}

// ── DequeueRX ──────────────────────────────────────────────────────────────
// Non-blocking pop from a queue. Returns byte count or 0.
// Called from Wifi timer thread — MUST be non-blocking.

int RelayServer::DequeueRX(std::queue<RelayRXEntry>& q,
                             u8* data, u64* timestamp)
{
    std::lock_guard<std::mutex> lk(RXMutex);
    if (q.empty()) return 0;
    RelayRXEntry& e = q.front();
    int sz = (int)e.Data.size();
    if (sz > 2048) sz = 2048;
    if (sz > 0) memcpy(data, e.Data.data(), sz);
    if (timestamp) *timestamp = e.Timestamp;
    q.pop();
    return sz;
}

// ── Host-side MPInterface calls ────────────────────────────────────────────
// These are called from Wifi.cpp via Platform::MP_* → MPInterface::*.
// ALL must be non-blocking (see header design notes).

int RelayServer::SendPacket(int inst, u8* data, int len, u64 timestamp)
{
    Log(LogLevel::Info, "KHMM: RelayServer::SendPacket len=%d numClients=%d\n",
        len, (int)Clients.size());
    // Type 0 = general frame (beacons, assoc, etc.)
    MPPacketHeader mph = {0x4946494E, 0, 0, (u32)len, timestamp};
    std::vector<u8> msg(sizeof(RelayMsgHeader) + sizeof(mph) + len);
    RelayMsgHeader hdr = {kRelayMagic, (u32)RMsg_MPPacket,
                          (u32)(sizeof(mph) + len)};
    memcpy(msg.data(), &hdr, sizeof(hdr));
    memcpy(msg.data() + sizeof(hdr), &mph, sizeof(mph));
    if (len) memcpy(msg.data() + sizeof(hdr) + sizeof(mph), data, len);

    std::lock_guard<std::mutex> lk(ClientsMutex);
    BroadcastRaw(msg.data(), (int)msg.size(), 0 /*exclude host AID*/);
    return len;
}

int RelayServer::RecvPacket(int inst, u8* data, u64* timestamp)
{
    return DequeueRX(RXQueue, data, timestamp);
}

int RelayServer::SendCmd(int inst, u8* data, int len, u64 timestamp)
{
    // Type 1 = CMD frame (host→all clients, time-critical)
    MPPacketHeader mph = {0x4946494E, 0, 1, (u32)len, timestamp};
    std::vector<u8> msg(sizeof(RelayMsgHeader) + sizeof(mph) + len);
    RelayMsgHeader hdr = {kRelayMagic, (u32)RMsg_MPPacket,
                          (u32)(sizeof(mph) + len)};
    memcpy(msg.data(), &hdr, sizeof(hdr));
    memcpy(msg.data() + sizeof(hdr), &mph, sizeof(mph));
    if (len) memcpy(msg.data() + sizeof(hdr) + sizeof(mph), data, len);

    std::lock_guard<std::mutex> lk(ClientsMutex);
    BroadcastRaw(msg.data(), (int)msg.size(), 0);
    return len;
}

int RelayServer::SendReply(int inst, u8* data, int len,
                            u64 timestamp, u16 aid)
{
    // Host's own reply — loopback into RXHostQueue
    std::lock_guard<std::mutex> lk(RXMutex);
    RelayRXEntry entry;
    if (data && len > 0)
        entry.Data.assign(data, data + len);
    entry.Timestamp = timestamp;
    entry.Type      = 2 | ((u32)aid << 16);
    entry.Aid       = aid;
    entry.SenderAID = 0;
    RXHostQueue.push(std::move(entry));
    return len;
}

int RelayServer::SendAck(int inst, u8* data, int len, u64 timestamp)
{
    // Type 3 = ACK frame (host→all clients)
    MPPacketHeader mph = {0x4946494E, 0, 3, (u32)len, timestamp};
    std::vector<u8> msg(sizeof(RelayMsgHeader) + sizeof(mph) + len);
    RelayMsgHeader hdr = {kRelayMagic, (u32)RMsg_MPPacket,
                          (u32)(sizeof(mph) + len)};
    memcpy(msg.data(), &hdr, sizeof(hdr));
    memcpy(msg.data() + sizeof(hdr), &mph, sizeof(mph));
    if (len) memcpy(msg.data() + sizeof(hdr) + sizeof(mph), data, len);

    std::lock_guard<std::mutex> lk(ClientsMutex);
    BroadcastRaw(msg.data(), (int)msg.size(), 0);
    return len;
}

int RelayServer::RecvHostPacket(int inst, u8* data, u64* timestamp)
{
    // Pull a general frame from clients (non-blocking).
    // Host's Wifi stack calls this from CheckRX(2) via USTimer.
    return DequeueRX(RXQueue, data, timestamp);
}

u16 RelayServer::RecvReplies(int inst, u8* packets,
                              u64 timestamp, u16 aidmask)
{
    // Called once per CMD cycle from ProcessTX phase-1 (NOT per timer tick).
    // Wait up to one CMD reply window for client replies to arrive.
    // CMD reply window = (10 + W_CmdReplyTime) * 8µs per client.
    // With our boosted W_CmdReplyTime=200, that's ~1.68ms per client.
    // We wait up to 12ms to cover 1-2 clients with network jitter.

    const u64 kReplyWaitUS = 12000; // 12ms
    u64 deadline = NowUS() + kReplyWaitUS;

    while (NowUS() < deadline)
    {
        {
            std::lock_guard<std::mutex> lk(RXMutex);
            if (!RXHostQueue.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    u16 ret = 0;
    {
        std::lock_guard<std::mutex> lk(RXMutex);
        Log(LogLevel::Info, "KHMM: RecvReplies aidmask=%04X queueSize=%d\n",
            aidmask, (int)RXHostQueue.size());
    }
    std::lock_guard<std::mutex> lk(RXMutex);
    while (!RXHostQueue.empty())
    {
        RelayRXEntry& e = RXHostQueue.front();
        Log(LogLevel::Info, "KHMM: RecvReplies draining Type=%08X Aid=%d aidmask=%04X\n",
            e.Type, e.Aid, aidmask);
        if ((e.Type & 0xFFFF) == 2)
        {
            u16 aid = e.Aid;
            if (aid > 0 && aid < 16 && (aidmask & (1 << aid)))
            {
                int sz = (int)e.Data.size();
                if (sz > 1024) sz = 1024;
                memcpy(&packets[(aid - 1) * 1024], e.Data.data(), sz);
                ret |= (1 << aid);
            }
        }
        RXHostQueue.pop();
    }
    return ret;
}

// ═══════════════════════════════════════════════════════════════════════════
// RelayClient
// ═══════════════════════════════════════════════════════════════════════════

RelayClient::RelayClient()
    : Sock((socket_t)INVALID_SOCKET)
    , Connected(false)
    , MyAID(-1)
    , MaxPlayers(4)
    , HostChannel(0)
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
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    Sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Sock == (socket_t)INVALID_SOCKET) return false;

    SetNoDelay(Sock);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u16)port);
    if (inet_pton(AF_INET, hostIP, &addr.sin_addr) != 1)
    {
        closesocket(Sock);
        Sock = (socket_t)INVALID_SOCKET;
        return false;
    }

    // 5-second non-blocking connect (PPSSPP style)
    SetNonBlocking(Sock, true);
    connect(Sock, (struct sockaddr*)&addr, sizeof(addr));

    fd_set fds;
    FD_ZERO(&fds); FD_SET(Sock, &fds);
    struct timeval tv = {5, 0};
    int r = select((int)Sock + 1, nullptr, &fds, nullptr, &tv);
    if (r <= 0)
    {
        Log(LogLevel::Warn, "Relay: connect timeout to %s:%d\n", hostIP, port);
        closesocket(Sock);
        Sock = (socket_t)INVALID_SOCKET;
        return false;
    }
    SetNonBlocking(Sock, false); // blocking for handshake

    // ── Send Hello ────────────────────────────────────────────────────────
    RelayHelloPayload hello;
    hello.Magic   = kRelayMagic;
    hello.Version = kRelayVersion;
    memset(hello.Code, 0, 6);
    memcpy(hello.Code, roomCode,
           std::min((size_t)6, strlen(roomCode)));
    memset(hello.Name, 0, 32);
    strncpy(hello.Name, playerName, 31);

    auto hellomsg = BuildMsg(RMsg_Hello, (u8*)&hello, sizeof(hello));
    if (!SendExact(Sock, hellomsg.data(), (int)hellomsg.size()))
    {
        closesocket(Sock); Sock = (socket_t)INVALID_SOCKET;
        return false;
    }

    // ── Receive ACK or NAK ────────────────────────────────────────────────
    tv = {5, 0};
    setsockopt(Sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    RelayMsgHeader ackhdr;
    if (!RecvExact(Sock, (u8*)&ackhdr, sizeof(ackhdr)))
    {
        closesocket(Sock); Sock = (socket_t)INVALID_SOCKET;
        return false;
    }

    if (ackhdr.Magic != kRelayMagic
        || ackhdr.Type == (u32)RMsg_HelloNak)
    {
        Log(LogLevel::Warn, "Relay: server rejected (bad code or full)\n");
        closesocket(Sock); Sock = (socket_t)INVALID_SOCKET;
        return false;
    }

    if (ackhdr.Type != (u32)RMsg_HelloAck
        || ackhdr.Length != sizeof(RelayHelloAckPayload))
    {
        closesocket(Sock); Sock = (socket_t)INVALID_SOCKET;
        return false;
    }

    RelayHelloAckPayload ack;
    if (!RecvExact(Sock, (u8*)&ack, sizeof(ack)))
    {
        closesocket(Sock); Sock = (socket_t)INVALID_SOCKET;
        return false;
    }

    MyAID       = ack.PlayerID;
    MaxPlayers  = ack.MaxPlayers;
    HostChannel = ack.HostChannel;

    // Remove recv timeout; RecvThread takes over
    tv = {0, 0};
    setsockopt(Sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    Connected = true;
    RecvThread = std::thread(&RelayClient::RecvLoop, this);

    Log(LogLevel::Info, "Relay: connected AID=%d hostCh=%d\n",
        MyAID, HostChannel);
    return true;
}

void RelayClient::Disconnect()
{
    if (!Connected && Sock == (socket_t)INVALID_SOCKET) return;

    Connected = false;

    if (Sock != (socket_t)INVALID_SOCKET)
    {
        auto msg = BuildMsg(RMsg_Disconnect);
        SendExact(Sock, msg.data(), (int)msg.size());
        closesocket(Sock);
        Sock = (socket_t)INVALID_SOCKET;
    }

    if (RecvThread.joinable())
        RecvThread.join();

#ifdef _WIN32
    WSACleanup();
#endif
}

// ── RecvLoop ───────────────────────────────────────────────────────────────
// Dedicated receive thread (like PPSSPP's friendFinder thread).
// Blocking reads on Sock → pushes into RXQueues.
// Wifi timer thread reads from RXQueues without ever touching the socket.

void RelayClient::RecvLoop()
{
    while (Connected && Sock != (socket_t)INVALID_SOCKET)
    {
        // Blocking read of header
        RelayMsgHeader hdr;
        if (!RecvExact(Sock, (u8*)&hdr, sizeof(hdr)))
        {
            Connected = false;
            break;
        }

        if (hdr.Magic != kRelayMagic)
        {
            Log(LogLevel::Warn, "Relay client: corrupt magic, disconnecting\n");
            Connected = false;
            break;
        }

        // Read payload
        u32 plen = hdr.Length;
        if (plen > 4096)
        {
            Log(LogLevel::Warn, "Relay client: oversized packet %u\n", plen);
            Connected = false;
            break;
        }

        std::vector<u8> payload(plen);
        if (plen > 0 && !RecvExact(Sock, payload.data(), (int)plen))
        {
            Connected = false;
            break;
        }

        switch ((RelayMsgType)hdr.Type)
        {
        case RMsg_PlayerList:
        {
            // Update peer table (PPSSPP friends list equivalent)
            if (payload.empty()) break;
            u8 count = payload[0];
            std::lock_guard<std::mutex> lk(PeersMutex);
            Peers.clear();
            for (int i = 0; i < count; i++)
            {
                int off = 1 + i * (int)sizeof(RelayPeer);
                if (off + (int)sizeof(RelayPeer) > (int)payload.size()) break;
                RelayPeer p;
                memcpy(&p, payload.data() + off, sizeof(RelayPeer));
                Peers.push_back(p);
            }
            break;
        }

        case RMsg_MPPacket:
        {
            if (plen < sizeof(MPPacketHeader)) break;
            MPPacketHeader* mph = (MPPacketHeader*)payload.data();
            u32 mptype = mph->Type & 0xFFFF;
            Log(LogLevel::Info, "KHMM: client RecvLoop got MPPacket type=%u len=%u\n",
                mptype, plen);

            RelayRXEntry entry;
            entry.Data.assign(payload.begin() + sizeof(MPPacketHeader),
                              payload.end());
            entry.Timestamp = mph->Timestamp;
            entry.Type      = mph->Type;
            entry.Aid       = (u16)(mph->Type >> 16);
            entry.SenderAID = (int)mph->SenderID;

            std::lock_guard<std::mutex> lk(RXMutex);
            if (mptype == 1 || mptype == 3)
            {
                // CMD (1) or ACK (3) from host → host queue
                // Client's CheckRX(2) drains this via RecvHostPacket
                RXHostQueue.push(std::move(entry));
            }
            else
            {
                // General (0) or reply echoes (2) → general queue
                RXQueue.push(std::move(entry));
            }
            break;
        }

        case RMsg_Disconnect:
            Log(LogLevel::Info, "Relay: server disconnected us\n");
            Connected = false;
            break;

        default:
            Log(LogLevel::Debug, "Relay client: unknown msg type %u\n",
                hdr.Type);
            break;
        }
    }
}

void RelayClient::Process()
{
    // RecvThread handles everything asynchronously.
}

// ── GetPeerList / GetNumPlayers ────────────────────────────────────────────
std::vector<RelayPeer> RelayClient::GetPeerList()
{
    std::lock_guard<std::mutex> lk(PeersMutex);
    return Peers;
}

int RelayClient::GetNumPlayers()
{
    std::lock_guard<std::mutex> lk(PeersMutex);
    return (int)Peers.size();
}

// ── DequeueRX ──────────────────────────────────────────────────────────────
int RelayClient::DequeueRX(std::queue<RelayRXEntry>& q,
                             u8* data, u64* timestamp)
{
    std::lock_guard<std::mutex> lk(RXMutex);
    if (q.empty()) return 0;
    RelayRXEntry& e = q.front();
    int sz = (int)e.Data.size();
    if (sz > 2048) sz = 2048;
    if (sz > 0) memcpy(data, e.Data.data(), sz);
    if (timestamp) *timestamp = e.Timestamp;
    q.pop();
    return sz;
}

// ── Client-side send helpers ───────────────────────────────────────────────
bool RelayClient::SendRaw(const u8* data, int len)
{
    std::lock_guard<std::mutex> lk(SendMutex);
    return SendExact(Sock, data, len);
}

bool RelayClient::SendMP(u32 mpType, u8* data, int len, u64 timestamp)
{
    if (!Connected || Sock == (socket_t)INVALID_SOCKET) return false;

    MPPacketHeader mph;
    mph.Magic     = 0x4946494E;
    mph.SenderID  = (u32)MyAID;
    mph.Type      = mpType;
    mph.Length    = (u32)len;
    mph.Timestamp = timestamp;

    std::vector<u8> msg(sizeof(RelayMsgHeader) + sizeof(mph) + len);
    RelayMsgHeader hdr = {kRelayMagic, (u32)RMsg_MPPacket,
                          (u32)(sizeof(mph) + len)};
    memcpy(msg.data(), &hdr, sizeof(hdr));
    memcpy(msg.data() + sizeof(hdr), &mph, sizeof(mph));
    if (len) memcpy(msg.data() + sizeof(hdr) + sizeof(mph), data, len);

    return SendRaw(msg.data(), (int)msg.size());
}

// ── Client-side MPInterface calls ──────────────────────────────────────────

int RelayClient::SendPacket(int inst, u8* data, int len, u64 timestamp)
{
    return SendMP(0, data, len, timestamp) ? len : 0;
}

int RelayClient::RecvPacket(int inst, u8* data, u64* timestamp)
{
    return DequeueRX(RXQueue, data, timestamp);
}

int RelayClient::SendCmd(int inst, u8* data, int len, u64 timestamp)
{
    return SendMP(1, data, len, timestamp) ? len : 0;
}

int RelayClient::SendReply(int inst, u8* data, int len,
                            u64 timestamp, u16 aid)
{
    u32 type = 2 | ((u32)aid << 16);
    return SendMP(type, data, len, timestamp) ? len : 0;
}

int RelayClient::SendAck(int inst, u8* data, int len, u64 timestamp)
{
    return SendMP(3, data, len, timestamp) ? len : 0;
}

int RelayClient::RecvHostPacket(int inst, u8* data, u64* timestamp)
{
    // Non-blocking — RecvThread has already pushed CMD/ACK frames here.
    // USTimer calls this up to 2083 times/frame; any blocking = fps loss.
    return DequeueRX(RXHostQueue, data, timestamp);
}

u16 RelayClient::RecvReplies(int inst, u8* packets,
                              u64 timestamp, u16 aidmask)
{
    // Clients don't call RecvReplies in normal DS MP flow
    // (only the host collects replies). Return 0 safely.
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Relay (MPInterface facade)
// ═══════════════════════════════════════════════════════════════════════════

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

    // Pass channel 0 for now; Wifi.cpp will update via SetHostChannel
    // once the RF registers are programmed (or we default to channel 7).
    // Channel doesn't affect routing — it's only used for the client
    // to adopt the host's channel early.
    Server = new RelayServer();
    if (!Server->Start(RoomCode, playerName, maxPlayers, 0))
    {
        delete Server; Server = nullptr;
        return false;
    }

    RelayModeActive = true;
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
        delete Client; Client = nullptr;
        return false;
    }

    RelayModeActive = true;
    return true;
}

void Relay::EndSession()
{
    if (Server) { Server->Stop(); delete Server; Server = nullptr; }
    if (Client) { Client->Disconnect(); delete Client; Client = nullptr; }
    memset(RoomCode, 0, sizeof(RoomCode));
    RelayModeActive = false;
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

int Relay::GetMyAID() const
{
    if (Client) return Client->GetMyAID();
    return 0; // host is always AID 0
}

std::vector<RelayPeer> Relay::GetPeerList()
{
    if (Server) return Server->GetPeerList();
    if (Client) return Client->GetPeerList();
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

void Relay::Begin(int inst) {}
void Relay::End(int inst)   {}

int Relay::SendPacket(int i, u8* d, int l, u64 t)
{ if (Server) return Server->SendPacket(i,d,l,t);
  if (Client) return Client->SendPacket(i,d,l,t); return 0; }

int Relay::RecvPacket(int i, u8* d, u64* t)
{ if (Server) return Server->RecvPacket(i,d,t);
  if (Client) return Client->RecvPacket(i,d,t); return 0; }

int Relay::SendCmd(int i, u8* d, int l, u64 t)
{ if (Server) return Server->SendCmd(i,d,l,t);
  if (Client) return Client->SendCmd(i,d,l,t); return 0; }

int Relay::SendReply(int i, u8* d, int l, u64 t, u16 aid)
{ if (Server) return Server->SendReply(i,d,l,t,aid);
  if (Client) return Client->SendReply(i,d,l,t,aid); return 0; }

int Relay::SendAck(int i, u8* d, int l, u64 t)
{ if (Server) return Server->SendAck(i,d,l,t);
  if (Client) return Client->SendAck(i,d,l,t); return 0; }

int Relay::RecvHostPacket(int i, u8* d, u64* t)
{ if (Server) return Server->RecvHostPacket(i,d,t);
  if (Client) return Client->RecvHostPacket(i,d,t); return 0; }

u16 Relay::RecvReplies(int i, u8* d, u64 t, u16 m)
{ if (Server) return Server->RecvReplies(i,d,t,m);
  if (Client) return Client->RecvReplies(i,d,t,m); return 0; }

} // namespace melonDS
