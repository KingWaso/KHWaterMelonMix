// NetplaySession.h
//
// Stage 1 session state machine: bootstraps all clients to an identical
// savestate, then runs the per-frame input-exchange loop that keeps each
// instance's local NiFi emulation in lockstep.
//
// This class does NOT know how bytes get from one machine to another —
// that's ITransport's job. Implement ITransport as a thin adapter over
// your existing RelayServer/RelayClient socket code; this class only
// calls SendTo/Broadcast and expects OnMessageReceived to be called back
// when bytes arrive. That separation is what lets you swap TCP-through-
// relay for direct P2P/UDP in Stage 2 without touching this file.
//
// ============================================================================
// INTEGRATION TODOs — search for "TODO(integration)" in NetplaySession.cpp.
// These are the only places that need to know about real melonDS types
// (ROM, Savestate, NDS, Emu, Input). Everything else here is
// self-contained and should compile as-is.
// ============================================================================

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "NetplayInputQueue.h"
#include "NetplayProtocol.h"

namespace Netplay {

enum class SessionState {
    Disconnected,
    Lobby,
    Bootstrap_AwaitingAcks,
    LockstepRunning,
};

enum class NackReason : uint8_t {
    CrcMismatch = 1,
    LoadFailed  = 2,
};

// Abstract transport — implement this over your existing relay socket
// code. All Send* calls are fire-and-forget from NetplaySession's
// perspective; retries/timeouts are handled at the NetplaySession level,
// not here.
class ITransport {
public:
    virtual ~ITransport() = default;

    // Send to one specific peer (used for per-recipient RomInfo, since
    // yourPlayerIndex differs per peer).
    virtual void SendTo(uint8_t peerIndex, MsgType type, const std::vector<uint8_t>& payload) = 0;

    // Send to all other peers (used for InputFrame broadcast).
    virtual void Broadcast(MsgType type, const std::vector<uint8_t>& payload) = 0;

    virtual uint8_t LocalPlayerIndex() const = 0;
    virtual uint8_t PlayerCount() const = 0;
};

class NetplaySession {
public:
    explicit NetplaySession(ITransport& transport);

    // Call once, on the host only, when all players in the lobby have
    // confirmed ready and the host wants to start the mission. Pauses
    // emulation, snapshots state, and sends RomInfo + InitStateBlob to
    // every peer.
    void Host_BeginBootstrap();

    // Feed raw bytes received from the transport here (one full message
    // at a time, header already stripped by your transport layer, OR see
    // NetplaySession::OnBytesReceived below if you'd rather hand this
    // class the raw growing recv buffer).
    void OnMessageReceived(MsgType type, const uint8_t* payload, size_t len);

    // Call once per emulated video frame, BEFORE stepping the emulator.
    // Sends this instance's local input for `frameNumber` and blocks
    // (with retry/backoff, not a hard stall) until every player's input
    // for (frameNumber - kInputDelayFrames) is available, then applies
    // all of them via the TODO(integration) hook in the .cpp.
    void PreFrameStep(uint32_t frameNumber);

    SessionState State() const { return m_state; }

    // Number of frames of input delay used to absorb network latency.
    // Stage 1: fixed. Stage 2: make this configurable per-session based
    // on measured ping.
    static constexpr uint32_t kInputDelayFrames = 4;

    // How long Host_BeginBootstrap waits for all InitStateAck before
    // resending to outstanding peers.
    static constexpr std::chrono::seconds kAckTimeout{10};

    // How long PreFrameStep will wait for a single player's input before
    // treating it as a stall worth logging (Stage 1: this is a soft
    // warning threshold, not a disconnect — the wait is retried, not
    // abandoned, since dropping input silently is exactly the kind of
    // thing that causes an undiagnosable desync later).
    static constexpr std::chrono::milliseconds kInputWaitWarnThreshold{500};

private:
    void HandleRomInfo(const RomInfoPayload& info);
    void HandleInitStateBlob(const uint8_t* data, size_t len);
    void HandleInitStateAck(uint8_t fromPlayer);
    void HandleInitStateNack(uint8_t fromPlayer, NackReason reason);
    void HandleInputFrame(const InputFramePayload& input);

    ITransport& m_transport;
    SessionState m_state = SessionState::Disconnected;
    InputQueue m_inputQueue;

    uint8_t m_localPlayerIndex = 0;
    uint32_t m_currentFrame = 0;

    // Bootstrap ack tracking (host side only)
    std::vector<bool> m_acksReceived;
    std::chrono::steady_clock::time_point m_bootstrapDeadline;
};

} // namespace Netplay
