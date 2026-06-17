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

/*
 * Relay.h — KHWaterMelonMix built-in relay server + client
 *
 * Architecture:
 *   HOST path:
 *     1. RelayServer thread starts inside the host's emulator process.
 *     2. It listens on a random TCP port (default range 7100-7200).
 *     3. A random 6-digit room code is generated and shown to the host.
 *     4. The host shares their IP + code with friends out-of-band.
 *     5. The server accepts client TCP connections, validates the room
 *        code in the handshake, then routes all MP packets between peers.
 *
 *   CLIENT path:
 *     1. User enters host IP + 6-digit code.
 *     2. RelayClient connects to host IP:port (port is embedded in code
 *        or fixed), sends the room code for validation.
 *     3. Server ACKs and assigns a player slot.
 *     4. All subsequent MP frames go through the relay server.
 *
 * The Relay class implements MPInterface so it drops in exactly like LAN.
 */

#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>

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
    #define INVALID_SOCKET (-1)
    #define SOCKET_ERROR   (-1)
    #define closesocket    close
#endif

#include "types.h"
#include "MPInterface.h"

namespace melonDS
{

// ─── Wire protocol ────────────────────────────────────────────────────────────
// Every TCP message starts with this fixed-size header followed by `Length`
// bytes of payload.

const u32 kRelayMagic   = 0x584D484B; // 'KHMX'
const u32 kRelayVersion = 1;
const int kRelayPort    = 7100;       // fixed port; clients always connect here

enum RelayMsgType : u32
{
    RMsg_Hello      = 1,  // client → server: { magic, version, code[6], name[32] }
    RMsg_HelloAck   = 2,  // server → client: { playerID (u8), numPlayers (u8) }
    RMsg_HelloNak   = 3,  // server → client: rejected (bad code / full)
    RMsg_PlayerList = 4,  // server → all:   { count, Player[count] }
    RMsg_MPPacket   = 5,  // any    → server → others: MPPacketHeader + data
    RMsg_Disconnect = 6,  // server → all:   { playerID }
};

struct RelayMsgHeader
{
    u32 Magic;      // kRelayMagic
    u32 Type;       // RelayMsgType
    u32 Length;     // bytes following this header
};

// ─── Relay player info ────────────────────────────────────────────────────────

struct RelayPlayer
{
    int      ID;
    char     Name[32];
    bool     Connected;  // fully handshaked
    u32      Address;    // remote IPv4 (network byte order)
    u32      Ping;
};

// ─── RelayServer ─────────────────────────────────────────────────────────────
// Runs inside the HOST's emulator process as a background thread.

class RelayServer
{
public:
    RelayServer();
    ~RelayServer();

    // Start listening. roomCode must be exactly 6 ASCII digits.
    // maxPlayers: 2..16. Returns false on bind failure.
    bool Start(const char* roomCode, const char* hostName, int maxPlayers);
    void Stop();

    bool IsRunning() const { return Running; }
    int  GetPort()   const { return Port; }

    // Called by the Relay MPInterface every video frame
    void Process();

    std::vector<RelayPlayer> GetPlayerList();
    int GetNumPlayers();
    int GetMaxPlayers() { return MaxPlayers; }

    // Packet injection: the host's own Wifi.cpp calls these via MPInterface
    // (same signatures as LAN / LocalMP)
    int  SendPacket   (int inst, u8* data, int len, u64 timestamp);
    int  RecvPacket   (int inst, u8* data, u64* timestamp);
    int  SendCmd      (int inst, u8* data, int len, u64 timestamp);
    int  SendReply    (int inst, u8* data, int len, u64 timestamp, u16 aid);
    int  SendAck      (int inst, u8* data, int len, u64 timestamp);
    int  RecvHostPacket(int inst, u8* data, u64* timestamp);
    u16  RecvReplies  (int inst, u8* data, u64 timestamp, u16 aidmask);

private:
    struct ClientConn
    {
        socket_t Sock;
        RelayPlayer Player;
        bool Handshaked;
        std::vector<u8> RecvBuf;
        u32 Ping;
    };

    socket_t       ListenSock;
    int            Port;
    int            MaxPlayers;
    char           RoomCode[7];   // 6 digits + NUL
    char           HostName[32];

    std::atomic<bool>       Running;
    std::thread             AcceptThread;
    std::mutex              ClientsMutex;
    std::vector<ClientConn> Clients;   // index 0 = slot reserved for host

    // RX queues for the host's Wifi stack (same shape as LAN.cpp)
    std::mutex              RXMutex;
    struct RXEntry { std::vector<u8> Data; u64 Timestamp; u32 Type; u16 Aid; };
    std::queue<RXEntry>     RXQueue;
    std::queue<RXEntry>     RXHostQueue;  // replies destined for host (type 2)

    void AcceptLoop();
    void ServiceClient(ClientConn& c);
    bool DoHandshake(ClientConn& c);
    void BroadcastPlayerList();
    void BroadcastPacket(const u8* header, int headerLen,
                         const u8* payload, int payloadLen,
                         int excludeSlot = -1);
    void SendToClient(ClientConn& c, const u8* data, int len);
    int  RecvGeneric(std::queue<RXEntry>& q, u8* data, u64* timestamp,
                     bool block, u32 typeFilter = 0xFFFFFFFF);
};

// ─── RelayClient ─────────────────────────────────────────────────────────────
// Runs inside the CLIENT's emulator process.

class RelayClient
{
public:
    RelayClient();
    ~RelayClient();

    // Connect to host. Returns false on failure.
    bool Connect(const char* hostIP, int port,
                 const char* roomCode, const char* playerName);
    void Disconnect();

    bool IsConnected() const { return Connected; }

    void Process();

    std::vector<RelayPlayer> GetPlayerList();
    int GetNumPlayers()  { return (int)Players.size(); }
    int GetMaxPlayers()  { return MaxPlayers; }

    // MPInterface hooks
    int  SendPacket   (int inst, u8* data, int len, u64 timestamp);
    int  RecvPacket   (int inst, u8* data, u64* timestamp);
    int  SendCmd      (int inst, u8* data, int len, u64 timestamp);
    int  SendReply    (int inst, u8* data, int len, u64 timestamp, u16 aid);
    int  SendAck      (int inst, u8* data, int len, u64 timestamp);
    int  RecvHostPacket(int inst, u8* data, u64* timestamp);
    u16  RecvReplies  (int inst, u8* data, u64 timestamp, u16 aidmask);

private:
    socket_t             Sock;
    std::atomic<bool>    Connected;
    int                  MaxPlayers;
    int                  MyPlayerID;

    std::thread          RecvThread;
    std::mutex           PlayersMutex;
    std::vector<RelayPlayer> Players;

    std::mutex           RXMutex;
    struct RXEntry { std::vector<u8> Data; u64 Timestamp; u32 Type; u16 Aid; };
    std::queue<RXEntry>  RXQueue;
    std::queue<RXEntry>  RXHostQueue;

    void RecvLoop();
    bool SendMsg(u32 type, const u8* payload, int len);
    int  RecvGeneric(std::queue<RXEntry>& q, u8* data, u64* timestamp,
                     bool block, u32 typeFilter = 0xFFFFFFFF);
};

// ─── Relay MPInterface ────────────────────────────────────────────────────────
// The single class that Wifi.cpp talks to.
// When hosting:  owns a RelayServer + a thin host-side relay connection.
// When joining:  owns a RelayClient.

class Relay : public MPInterface
{
public:
    Relay() noexcept;
    ~Relay() noexcept override;

    // Host a game. Generates a 6-digit code internally.
    // Returns false if the server couldn't bind.
    bool HostGame(const char* playerName, int maxPlayers);

    // Join a game.
    bool JoinGame(const char* playerName, const char* hostIP,
                  const char* roomCode);

    void EndSession();

    bool IsHosting()   const { return Server != nullptr; }
    bool IsConnected() const;

    // The 6-digit room code (valid after HostGame succeeds)
    const char* GetRoomCode() const { return RoomCode; }
    int         GetPort()     const;

    std::vector<RelayPlayer> GetPlayerList();
    int GetNumPlayers();
    int GetMaxPlayers();

    // MPInterface
    void Process() override;
    void Begin(int inst) override;
    void End(int inst) override;

    int  SendPacket   (int inst, u8* data, int len, u64 timestamp) override;
    int  RecvPacket   (int inst, u8* data, u64* timestamp) override;
    int  SendCmd      (int inst, u8* data, int len, u64 timestamp) override;
    int  SendReply    (int inst, u8* data, int len, u64 timestamp, u16 aid) override;
    int  SendAck      (int inst, u8* data, int len, u64 timestamp) override;
    int  RecvHostPacket(int inst, u8* data, u64* timestamp) override;
    u16  RecvReplies  (int inst, u8* data, u64 timestamp, u16 aidmask) override;

private:
    char RoomCode[7];

    // Exactly one of these is non-null at a time
    RelayServer* Server;
    RelayClient* Client;

    static std::string GenerateCode();
};

// ─── Utility ──────────────────────────────────────────────────────────────────
// Get the machine's primary outbound IPv4 as a dotted-decimal string.
// Used by the UI to display "Your IP address: x.x.x.x" to the host.
std::string GetLocalIPAddress();

} // namespace melonDS

#endif // RELAY_H
