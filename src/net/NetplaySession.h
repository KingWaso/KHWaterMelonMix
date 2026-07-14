// NetplaySession.h
//
// Revision: replaces Stage 1's blocking-wait input model with rollback
// reconciliation, matching the technique independently arrived at in
// melonDS PR #2453 (unmerged, prototype-quality, but the rollback
// approach itself is sound): when a player's input for a frame hasn't
// arrived yet, speculate using their last known input, keep stepping,
// and if the real input turns out different once it arrives, roll back
// to a saved state and re-simulate forward with corrected inputs.
//
// This trades "occasionally re-simulate a few frames" for "never stall
// the game waiting on the network" — much better for smoothness, at the
// cost of NetplaySession now needing to own frame-stepping and state
// capture directly (via IEmulatorHost) rather than just injecting input
// before someone else steps the frame.
//
// ============================================================================
// INTEGRATION: IEmulatorHost is the only interface this file needs
// implemented against real melonDS types. See MelonDSEmulatorHost in
// NetplaySession.cpp for a concrete implementation built on confirmed
// melonDS APIs (NDS::DoSavestate, NDS::SetKeyMask, SPI TSC touch calls).
// The one piece still marked TODO(integration) is which function
// actually advances one emulated frame in your fork's main loop — wire
// that to whatever your EmuInstance/main loop currently calls.
// ============================================================================

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
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

// Abstract transport — implement over your existing relay socket code.
class ITransport {
public:
    virtual ~ITransport() = default;
    virtual void SendTo(uint8_t peerIndex, MsgType type, const std::vector<uint8_t>& payload) = 0;
    virtual void Broadcast(MsgType type, const std::vector<uint8_t>& payload) = 0;
    virtual uint8_t LocalPlayerIndex() const = 0;
    virtual uint8_t PlayerCount() const = 0;
};

// Abstract emulator host — the only thing standing between this file and
// real melonDS calls. Implement once (see MelonDSEmulatorHost in the
// .cpp) and NetplaySession never needs to change when your fork's
// internals change.
class IEmulatorHost {
public:
    virtual ~IEmulatorHost() = default;

    // Serialize the full current emulator state (== NDS::DoSavestate with
    // save=true under the hood).
    virtual void SaveState(std::vector<uint8_t>& outBuffer) = 0;

    // Restore state previously produced by SaveState (== DoSavestate with
    // save=false). Returns false on CRC/format mismatch.
    virtual bool LoadState(const std::vector<uint8_t>& buffer) = 0;

    // Apply one player's input for the frame about to be stepped.
    virtual void ApplyInput(uint8_t playerIndex, uint16_t keyMask,
                             bool touchActive, int16_t touchX, int16_t touchY) = 0;

    // Advance the emulator by exactly one video frame. Called both for
    // normal forward progress and when re-simulating after a rollback —
    // must not skip audio/video output logic your fork depends on for
    // determinism (see the feasibility writeup's determinism caveats:
    // avoid skipping rendering on any instance).
    virtual void StepFrame() = 0;

    virtual uint32_t RomCRC32() const = 0;
    virtual uint32_t RomSizeBytes() const = 0;
};

class NetplaySession {
public:
    NetplaySession(ITransport& transport, IEmulatorHost& host);

    void Host_BeginBootstrap();
    void OnMessageReceived(MsgType type, const uint8_t* payload, size_t len);

    // Call once per real-time tick from your main loop. Internally:
    //  1. Applies any pending rollback (reconciles late-arriving input
    //     against what was speculated).
    //  2. Steps exactly one new frame forward using best-available
    //     input (real if we have it, repeated-last-known if not).
    // Unlike the previous blocking design, this NEVER stalls — a frame
    // is always produced. Correctness is recovered after the fact via
    // rollback, not by waiting before the fact.
    void Tick();

    SessionState State() const { return m_state; }

    static constexpr uint32_t kInputDelayFrames = 4;

    // How many past frames we keep savestates for. Must be >= the worst
    // realistic network delay you want to tolerate; each entry costs one
    // full savestate's worth of memory, so this is a real memory/CPU
    // tradeoff, not just a number to bump freely.
    static constexpr uint32_t kRollbackWindowFrames = 60; // ~1s at 60fps

    static constexpr std::chrono::seconds kAckTimeout{10};

private:
    void HandleRomInfo(const RomInfoPayload& info);
    void HandleInitStateBlob(const uint8_t* data, size_t len);
    void HandleInitStateAck(uint8_t fromPlayer);
    void HandleInitStateNack(uint8_t fromPlayer, NackReason reason);
    void HandleInputFrame(const InputFramePayload& input);

    InputFramePayload PredictInput(uint8_t playerIndex, uint32_t frameNumber);
    void RecordAppliedInput(uint32_t frameNumber, uint8_t playerIndex,
                             const InputFramePayload& input, bool wasSpeculative);
    void ReconcileIfNeeded();
    void PruneOldHistory();

    ITransport& m_transport;
    IEmulatorHost& m_host;
    SessionState m_state = SessionState::Disconnected;
    InputQueue m_inputQueue;

    uint8_t m_localPlayerIndex = 0;
    uint32_t m_currentFrame = 0;

    // Ring buffer: frameNumber -> savestate taken BEFORE that frame was
    // stepped. Lets us rewind to "just before frame N" and re-run
    // forward with corrected inputs.
    std::map<uint32_t, std::vector<uint8_t>> m_stateRingBuffer;

    // What we actually applied per (frame, player), and whether it was a
    // guess. Needed to detect "the real input differs from what we
    // speculated" when a late InputFrame arrives.
    struct AppliedInput {
        InputFramePayload input;
        bool wasSpeculative;
    };
    std::map<uint32_t, std::map<uint8_t, AppliedInput>> m_appliedHistory;

    // Last known real (non-speculative) input per player, used as the
    // prediction when a frame's real input hasn't arrived yet.
    std::map<uint8_t, InputFramePayload> m_lastKnownInput;

    // Set by HandleInputFrame when a late input contradicts what was
    // speculated; consumed by Tick()/ReconcileIfNeeded().
    bool m_rollbackPending = false;
    uint32_t m_rollbackTargetFrame = 0;

    std::vector<bool> m_acksReceived;
    std::chrono::steady_clock::time_point m_bootstrapDeadline;
};

} // namespace Netplay
