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

#ifndef RELAY_H
#define RELAY_H

#include <string>
#include <vector>
#include <queue>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cstdint>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET socket_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    typedef int socket_t;
    #ifndef INVALID_SOCKET
        #define INVALID_SOCKET (-1)
    #endif
    #ifndef closesocket
        #define closesocket close
    #endif
#endif

#include "types.h"
#include "MPInterface.h"

namespace melonDS
{

// ─── Wire protocol ──────────────────────────────────────────────────────────

const u32 kRelayMagic   = 0x584D484B; // 'KHMX'
const u32 kRelayVersion = 2;
const int kRelayPort    = 7100;

enum RelayMsgType : u32
{
    RMsg_Hello      = 1,
    RMsg_HelloAck   = 2,
    RMsg_HelloNak   = 3,
    RMsg_PlayerList = 4,
    RMsg_Disconnect = 6,
    RMsg_MPPacket   = 5,
};

struct RelayMsgHeader
{
    u32 Magic;
    u32 Type;
    u32 Length;
};

struct RelayHelloPayload
{
    u32  Magic;
    u32  Version;
    char Code[6];
    char Name[32];
};

struct RelayHelloAckPayload
{
    u8 PlayerID;
    u8 MaxPlayers;
    u8 HostChannel;
    u8 _pad;
};

// ─── Peer info ───────────────────────────────────────────────────────────────

struct RelayPeer
{
    int      AID;
    char     Name[32];
    bool     Connected;
    u32      Address;
    u32      Ping;
    u64      LastRecvUS;
};

// ─── RX queue entry ──────────────────────────────────────────────────────────

struct RelayRXEntry
{
    std::vector<u8> Data;
    u64             Timestamp;
    u32             Type;
    u16             Aid;
    int             SenderAID;
};

// ─── RelayServer ─────────────────────────────────────────────────────────────

class RelayServer
{
public:
    RelayServer();
    ~RelayServer();

    bool Start(const char* roomCode, const char* hostName,
               int maxPlayers, u8 hostChannel);
    void Stop();

    bool IsRunning()    const { return Running.load(); }
    int  GetPort()      const { return Port; }
    u8   GetChannel()   const { return HostChannel; }
    int  GetMaxPlayers() { return MaxPlayers; }

    void Process();

    std::vector<RelayPeer> GetPeerList();
    int GetNumPlayers();

    int  SendPacket    (int inst, u8* data, int len, u64 timestamp);
    int  RecvPacket    (int inst, u8* data, u64* timestamp);
    int  SendCmd       (int inst, u8* data, int len, u64 timestamp);
    int  SendReply     (int inst, u8* data, int len, u64 timestamp, u16 aid);
    int  SendAck       (int inst, u8* data, int len, u64 timestamp);
    int  RecvHostPacket(int inst, u8* data, u64* timestamp);
    u16  RecvReplies   (int inst, u8* packets, u64 timestamp, u16 aidmask);

private:
    struct ClientConn
    {
        socket_t   Sock       = (socket_t)INVALID_SOCKET;
        RelayPeer  Peer       = {};
        bool       Handshaked = false;
    };

    socket_t              ListenSock;
    int                   Port;
    int                   MaxPlayers;
    u8                    HostChannel;
    char                  RoomCode[7];
    char                  HostName[32];

    std::atomic<bool>     Running;
    std::thread           AcceptThread;

    std::mutex            ClientsMutex;
    std::vector<ClientConn> Clients;

    // RX queues — written by AcceptThread, read by Wifi timer thread
    std::mutex               RXMutex;
    std::queue<RelayRXEntry> RXQueue;
    std::queue<RelayRXEntry> RXHostQueue;

    // KHWaterMelonMix: reply pre-cache for non-blocking RecvReplies.
    // AcceptThread writes here as replies arrive (inside RXMutex).
    // RecvReplies reads and clears it instantly — no waiting.
    u8   CachedReplies[16][1024];  // indexed by AID 1..15
    u16  CachedReplyMask;          // bitmask of AIDs with valid data

    void AcceptLoop();
    bool DoHandshake(ClientConn& c);
    void ServiceClient(ClientConn& c);
    void DispatchMPPacket(ClientConn& sender, const u8* payload, u32 len);
    void BroadcastPlayerList();
    void BroadcastRaw(const u8* data, int len, int excludeAID = -1);
    bool SendRaw(socket_t s, const u8* data, int len);
    int  DequeueRX(std::queue<RelayRXEntry>& q, u8* data, u64* timestamp);
    bool LastBeaconStateWasOpen = true; // true=0x01, false=0x00
};

// ─── RelayClient ─────────────────────────────────────────────────────────────

class RelayClient
{
public:
    RelayClient();
    ~RelayClient();

    bool Connect(const char* hostIP, int port,
                 const char* roomCode, const char* playerName);
    void Disconnect();

    bool IsConnected()   const { return Connected.load(); }
    int  GetMyAID()      const { return MyAID; }
    int  GetMaxPlayers() const { return MaxPlayers; }
    u8   GetHostChannel() const { return HostChannel; }

    void Process();

    std::vector<RelayPeer> GetPeerList();
    int GetNumPlayers();

    int  SendPacket    (int inst, u8* data, int len, u64 timestamp);
    int  RecvPacket    (int inst, u8* data, u64* timestamp);
    int  SendCmd       (int inst, u8* data, int len, u64 timestamp);
    int  SendReply     (int inst, u8* data, int len, u64 timestamp, u16 aid);
    int  SendAck       (int inst, u8* data, int len, u64 timestamp);
    int  RecvHostPacket(int inst, u8* data, u64* timestamp);
    u16  RecvReplies   (int inst, u8* packets, u64 timestamp, u16 aidmask);

private:
    socket_t              Sock;
    std::atomic<bool>     Connected;
    int                   MyAID;
    int                   MaxPlayers;
    u8                    HostChannel;

    std::thread           RecvThread;

    std::mutex            PeersMutex;
    std::vector<RelayPeer> Peers;

    std::mutex               RXMutex;
    std::queue<RelayRXEntry> RXQueue;
    std::queue<RelayRXEntry> RXHostQueue;

    std::mutex            SendMutex;

    void RecvLoop();
    bool SendRaw(const u8* data, int len);
    bool SendMP(u32 mpType, u8* data, int len, u64 timestamp);
    int  DequeueRX(std::queue<RelayRXEntry>& q, u8* data, u64* timestamp);
};

// ─── Relay MPInterface ───────────────────────────────────────────────────────

class Relay : public MPInterface
{
public:
    Relay() noexcept;
    ~Relay() noexcept override;

    bool HostGame(const char* playerName, int maxPlayers);
    bool JoinGame(const char* playerName, const char* hostIP,
                  const char* roomCode);
    void EndSession();

    bool IsHosting()    const { return Server != nullptr; }
    bool IsConnected()  const;

    const char* GetRoomCode() const { return RoomCode; }
    int         GetPort()     const;
    int         GetMyAID()    const;

    std::vector<RelayPeer> GetPeerList();
    int GetNumPlayers();
    int GetMaxPlayers();

    void Process() override;
    void Begin(int inst) override;
    void End(int inst)   override;

    int  SendPacket    (int inst, u8* data, int len, u64 timestamp) override;
    int  RecvPacket    (int inst, u8* data, u64* timestamp)         override;
    int  SendCmd       (int inst, u8* data, int len, u64 timestamp) override;
    int  SendReply     (int inst, u8* data, int len, u64 timestamp, u16 aid) override;
    int  SendAck       (int inst, u8* data, int len, u64 timestamp) override;
    int  RecvHostPacket(int inst, u8* data, u64* timestamp)         override;
    u16  RecvReplies   (int inst, u8* packets, u64 timestamp, u16 aidmask)   override;

private:
    char          RoomCode[7];
    RelayServer*  Server;
    RelayClient*  Client;

    static std::string GenerateCode();
};

// ─── Utility ─────────────────────────────────────────────────────────────────

std::string GetLocalIPAddress();
extern bool RelayModeActive;

} // namespace melonDS
#endif // RELAY_H
