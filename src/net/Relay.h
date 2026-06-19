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
 * Relay.h — KHWaterMelonMix built-in relay server + client (v2)
 *
 * Design principles (PPSSPP-inspired):
 *
 *  1. SEPARATE CONTROL FROM DATA
 *     The TCP connection handles both handshake (control) and MP packets
 *     (data), but they are processed on separate threads so the control
 *     handshake never blocks the emulator's Wifi timer thread.
 *
 *  2. PEER TABLE (like PPSSPP's `friends` list)
 *     Every connected peer is tracked in a PeerInfo table keyed by AID
 *     (the DS association ID, 1..15). All routing decisions go through
 *     this table. Unknown peers are logged and discarded, never crash.
 *
 *  3. FULLY ASYNC RECEIVE (like PPSSPP's `friendFinder` thread)
 *     Each client connection gets a dedicated RecvWorker thread that reads
 *     from the TCP socket and pushes into a thread-safe RXQueue. The Wifi
 *     timer thread only reads from that queue — it never blocks on I/O.
 *
 *  4. NEVER BLOCK THE WIFI THREAD
 *     USTimer fires every 8µs (kTimerInterval). Any blocking call in
 *     RecvHostPacket multiplies: 1ms × 2083 calls/frame = 2083ms/frame.
 *     Every function callable from Wifi.cpp is non-blocking.
 *
 *  5. DS-SPECIFIC: HONOUR THE CMD REPLY WINDOW
 *     Unlike PSP adhoc (connectionless, loss-tolerant), DS local MP has a
 *     hardware-enforced host-CMD → client-reply lockstep. The relay routes
 *     reply packets to the host's RXHostQueue as fast as possible so they
 *     arrive before the host's ProcessTX phase-2 window expires.
 *     RecvReplies() on the host side waits up to one CMD cycle for replies.
 *
 *  6. CHANNEL BYPASS
 *     The DS wifi stack normally drops frames whose channel doesn't match
 *     CurChannel. Over the relay, CurChannel on the client may be 0 at
 *     first. Relay packets carry the host's channel; the client adopts it
 *     on first reception. Wifi.cpp skips the channel check for relay frames.
 */

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

// ─── Wire protocol ─────────────────────────────────────────────────────────
// Every TCP message: RelayMsgHeader (12 bytes) + payload (Length bytes).
// All multi-byte fields are native byte order (both peers run the emulator).

const u32 kRelayMagic   = 0x584D484B; // 'KHMX'
const u32 kRelayVersion = 2;           // bump from v1 to distinguish rebuilds
const int kRelayPort    = 7100;

enum RelayMsgType : u32
{
    // Handshake (control plane)
    RMsg_Hello      = 1,   // client→server: HelloPayload
    RMsg_HelloAck   = 2,   // server→client: HelloAckPayload
    RMsg_HelloNak   = 3,   // server→client: empty (bad code / full)

    // Session management (control plane)
    RMsg_PlayerList = 4,   // server→all: PlayerListPayload
    RMsg_Disconnect = 6,   // either direction: empty

    // DS MP data plane
    // Payload = MPPacketHeader (from MPInterface.h) + frame data
    RMsg_MPPacket   = 5,
};

struct RelayMsgHeader
{
    u32 Magic;    // kRelayMagic
    u32 Type;     // RelayMsgType
    u32 Length;   // bytes of payload after this header
};

// Handshake payloads
struct RelayHelloPayload
{
    u32  Magic;      // kRelayMagic (redundant check)
    u32  Version;    // kRelayVersion
    char Code[6];    // room code (ASCII digits, not NUL-terminated)
    char Name[32];   // player display name (NUL-terminated)
};

struct RelayHelloAckPayload
{
    u8 PlayerID;    // AID assigned by server (1..15)
    u8 MaxPlayers;  // max slots for this session
    u8 HostChannel; // DS wifi channel the host is using (0 = unknown)
    u8 _pad;
};

// ─── Peer info ─────────────────────────────────────────────────────────────
// Mirrors PPSSPP's SceNetAdhocctlPeerInfo / friends list.
// Keyed by AID (1..15). AID 0 = host (always present on server side).

struct RelayPeer
{
    int      AID;           // 1..15 (0 = host slot, local only)
    char     Name[32];      // display name
    bool     Connected;     // fully handshaked and active
    u32      Address;       // remote IPv4 (network byte order), 0 = local
    u32      Ping;          // last measured RTT in ms
    u64      LastRecvUS;    // wall-clock µs of last received packet
};

// ─── RX queue entry ────────────────────────────────────────────────────────
struct RelayRXEntry
{
    std::vector<u8> Data;       // frame payload (after MPPacketHeader)
    u64             Timestamp;  // DS USTimestamp from sender
    u32             Type;       // MPPacketHeader::Type (full word, AID in high 16)
    u16             Aid;        // extracted AID (Type >> 16)
    int             SenderAID;  // which peer sent this
};

// ─── RelayServer ───────────────────────────────────────────────────────────
// Runs inside the HOST's emulator process.
// AcceptThread: select() on ListenSock + all client sockets, 1ms timeout.
// Per-client RecvWorker threads are NOT used server-side; instead the
// AcceptThread services all clients via a single select() loop, giving
// us PPSSPP-style "non-blocking poll" without per-thread overhead.

class RelayServer
{
public:
    RelayServer();
    ~RelayServer();

    bool Start(const char* roomCode, const char* hostName,
               int maxPlayers, u8 hostChannel);
    void Stop();

    bool IsRunning() const { return Running.load(); }
    int  GetPort()   const { return Port; }
    u8   GetChannel() const { return HostChannel; }

    void Process();  // called from emulator main thread each video frame

    std::vector<RelayPeer> GetPeerList();
    int GetNumPlayers();
    int GetMaxPlayers() { return MaxPlayers; }

    // Called from Wifi.cpp via MPInterface (all non-blocking)
    int  SendPacket    (int inst, u8* data, int len, u64 timestamp);
    int  RecvPacket    (int inst, u8* data, u64* timestamp);
    int  SendCmd       (int inst, u8* data, int len, u64 timestamp);
    int  SendReply     (int inst, u8* data, int len, u64 timestamp, u16 aid);
    int  SendAck       (int inst, u8* data, int len, u64 timestamp);
    int  RecvHostPacket(int inst, u8* data, u64* timestamp);
    u16  RecvReplies   (int inst, u8* packets, u64 timestamp, u16 aidmask);

private:
    // ── Per-connection state ──────────────────────────────────────────────
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

    // ClientsMutex guards Clients vector and all Peer fields
    std::mutex            ClientsMutex;
    std::vector<ClientConn> Clients;

    // RX queues (written by AcceptThread, read by Wifi timer thread)
    std::mutex            RXMutex;
    std::queue<RelayRXEntry> RXQueue;      // general frames (type 0)
    std::queue<RelayRXEntry> RXHostQueue;  // client replies (type 2)

    // ── Internal methods ─────────────────────────────────────────────────
    void AcceptLoop();
    bool DoHandshake(ClientConn& c);
    void ServiceClient(ClientConn& c);
    void DispatchMPPacket(ClientConn& sender,
                          const u8* payload, u32 len);

    void BroadcastPlayerList();
    void BroadcastRaw(const u8* data, int len, int excludeAID = -1);
    bool SendRaw(socket_t s, const u8* data, int len);

    // Non-blocking queue drain (called from Wifi timer thread only)
    int  DequeueRX(std::queue<RelayRXEntry>& q,
                   u8* data, u64* timestamp);
};

// ─── RelayClient ───────────────────────────────────────────────────────────
// Runs inside each CLIENT's emulator process.
// RecvThread: blocking reads on the TCP socket, pushes to RXQueues.
// All methods called from Wifi.cpp are non-blocking queue reads.

class RelayClient
{
public:
    RelayClient();
    ~RelayClient();

    bool Connect(const char* hostIP, int port,
                 const char* roomCode, const char* playerName);
    void Disconnect();

    bool IsConnected()  const { return Connected.load(); }
    int  GetMyAID()     const { return MyAID; }
    int  GetMaxPlayers() const { return MaxPlayers; }
    u8   GetHostChannel() const { return HostChannel; }

    void Process();

    std::vector<RelayPeer> GetPeerList();
    int GetNumPlayers();

    // Called from Wifi.cpp via MPInterface (all non-blocking)
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

    // RecvThread: blocking reads from Sock, pushes to RXQueues
    std::thread           RecvThread;

    // Peer table (mirrors PPSSPP friends list)
    std::mutex            PeersMutex;
    std::vector<RelayPeer> Peers;

    // RX queues (written by RecvThread, read by Wifi timer thread)
    std::mutex            RXMutex;
    std::queue<RelayRXEntry> RXQueue;      // beacons, general (type 0)
    std::queue<RelayRXEntry> RXHostQueue;  // CMD + ACK from host (type 1,3)

    // SendMutex protects all sends to Sock from Wifi timer thread
    std::mutex            SendMutex;

    // ── Internal methods ─────────────────────────────────────────────────
    void RecvLoop();
    bool SendRaw(const u8* data, int len);
    bool SendMP(u32 mpType, u8* data, int len, u64 timestamp);

    int  DequeueRX(std::queue<RelayRXEntry>& q,
                   u8* data, u64* timestamp);
};

// ─── Relay MPInterface ─────────────────────────────────────────────────────
// The class Wifi.cpp talks to via Platform::MP_* calls.
// Owns either a RelayServer (hosting) or RelayClient (joining).

class Relay : public MPInterface
{
public:
    Relay() noexcept;
    ~Relay() noexcept override;

    // UI entry points
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

    // MPInterface implementation
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

// ─── Utility ───────────────────────────────────────────────────────────────
// Returns the machine's primary outbound IPv4 as a dotted-decimal string.
std::string GetLocalIPAddress();

// Set by Relay when a session is active; read by Wifi.cpp to bypass
// the channel check and boost W_CmdReplyTime.
extern bool RelayModeActive;

} // namespace melonDS
#endif // RELAY_H
