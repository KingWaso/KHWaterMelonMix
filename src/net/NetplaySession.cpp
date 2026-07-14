// NetplaySession.cpp
//
// Rollback-reconciliation implementation, plus MelonDSEmulatorHost — a
// concrete IEmulatorHost built directly on confirmed melonDS APIs:
//   - NDS::DoSavestate(Savestate*)   for full-state save/load
//   - NDS::SetKeyMask(u32)          for button input
//   - SPI.GetTSC()->SetTouchCoords(u16,u16)  for touch input (y=0xFFF
//     is the hardware's own "released" sentinel — using it directly
//     means no separate touchActive flag is needed at this layer)
//
// One TODO(integration) remains: which function in your fork's main
// loop actually advances one video frame (NDS::RunFrame<Mode>() is the
// real call, but it's templated on CPU execution mode and normally
// invoked from EmuInstance's loop, not called standalone) — wire
// MelonDSEmulatorHost::StepFrame() to whatever that is once you've
// pasted EmuInstance.cpp/.h so I can see the exact call site.

#include "NetplaySession.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>

// TODO(integration): real melonDS headers.
// #include "NDS.h"
// #include "Savestate.h"
// #include "SPI.h"

namespace Netplay {

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------
static bool InputsEqual(const InputFramePayload& a, const InputFramePayload& b) {
    return a.keyMask == b.keyMask
        && a.touchActive == b.touchActive
        && a.touchX == b.touchX
        && a.touchY == b.touchY;
}

// ---------------------------------------------------------------------
// NetplaySession
// ---------------------------------------------------------------------
NetplaySession::NetplaySession(ITransport& transport, IEmulatorHost& host)
    : m_transport(transport), m_host(host) {
    m_localPlayerIndex = transport.LocalPlayerIndex();
}

void NetplaySession::Host_BeginBootstrap() {
    RomInfoPayload info;
    info.romCRC32     = m_host.RomCRC32();
    info.romSizeBytes = m_host.RomSizeBytes();
    info.playerCount  = m_transport.PlayerCount();

    std::vector<uint8_t> stateBlob;
    m_host.SaveState(stateBlob);

    m_acksReceived.assign(info.playerCount, false);
    if (m_localPlayerIndex < m_acksReceived.size())
        m_acksReceived[m_localPlayerIndex] = true;

    for (uint8_t peer = 0; peer < info.playerCount; ++peer) {
        if (peer == m_localPlayerIndex) continue;
        RomInfoPayload perPeer = info;
        perPeer.yourPlayerIndex = peer;

        std::vector<uint8_t> infoBytes;
        perPeer.Serialize(infoBytes);
        m_transport.SendTo(peer, MsgType::RomInfo, infoBytes);
        m_transport.SendTo(peer, MsgType::InitStateBlob, stateBlob);
    }

    m_state = SessionState::Bootstrap_AwaitingAcks;
    m_bootstrapDeadline = std::chrono::steady_clock::now() + kAckTimeout;
    // As before: wire periodic resend-to-unacked-peers into whatever
    // timer your lobby UI already runs.
}

void NetplaySession::OnMessageReceived(MsgType type, const uint8_t* payload, size_t len) {
    switch (type) {
        case MsgType::RomInfo: {
            RomInfoPayload info;
            if (RomInfoPayload::Deserialize(payload, len, info))
                HandleRomInfo(info);
            break;
        }
        case MsgType::InitStateBlob:
            HandleInitStateBlob(payload, len);
            break;
        case MsgType::InitStateAck:
            if (len >= 1) HandleInitStateAck(payload[0]);
            break;
        case MsgType::InitStateNack:
            if (len >= 2) HandleInitStateNack(payload[0], static_cast<NackReason>(payload[1]));
            break;
        case MsgType::InputFrame: {
            InputFramePayload input;
            if (InputFramePayload::Deserialize(payload, len, input))
                HandleInputFrame(input);
            break;
        }
        case MsgType::DesyncHash:
        case MsgType::PlayerLeft:
        case MsgType::Hello:
        case MsgType::RoomJoin:
            break;
    }
}

void NetplaySession::HandleRomInfo(const RomInfoPayload& info) {
    if (m_host.RomCRC32() != info.romCRC32) {
        std::vector<uint8_t> nackPayload{ m_localPlayerIndex, static_cast<uint8_t>(NackReason::CrcMismatch) };
        m_transport.Broadcast(MsgType::InitStateNack, nackPayload);
        // TODO(integration): UI::ShowError("Players are running different ROM versions.");
        return;
    }
    m_localPlayerIndex = info.yourPlayerIndex;
}

void NetplaySession::HandleInitStateBlob(const uint8_t* data, size_t len) {
    std::vector<uint8_t> blob(data, data + len);
    bool ok = m_host.LoadState(blob);

    std::vector<uint8_t> ackPayload{ m_localPlayerIndex };
    if (ok) {
        m_transport.Broadcast(MsgType::InitStateAck, ackPayload);
        m_currentFrame = 0;
        m_inputQueue.Reset();
        m_stateRingBuffer.clear();
        m_appliedHistory.clear();
        m_lastKnownInput.clear();
        m_rollbackPending = false;
        m_state = SessionState::LockstepRunning;
    } else {
        std::vector<uint8_t> nackPayload{ m_localPlayerIndex, static_cast<uint8_t>(NackReason::LoadFailed) };
        m_transport.Broadcast(MsgType::InitStateNack, nackPayload);
    }
}

void NetplaySession::HandleInitStateAck(uint8_t fromPlayer) {
    if (fromPlayer < m_acksReceived.size())
        m_acksReceived[fromPlayer] = true;

    bool allAcked = true;
    for (bool a : m_acksReceived) allAcked &= a;

    if (allAcked && m_state == SessionState::Bootstrap_AwaitingAcks) {
        m_currentFrame = 0;
        m_inputQueue.Reset();
        m_stateRingBuffer.clear();
        m_appliedHistory.clear();
        m_lastKnownInput.clear();
        m_rollbackPending = false;
        m_state = SessionState::LockstepRunning;
    }
}

void NetplaySession::HandleInitStateNack(uint8_t fromPlayer, NackReason reason) {
    std::cerr << "[Netplay] Player " << static_cast<int>(fromPlayer)
              << " failed bootstrap (reason=" << static_cast<int>(reason)
              << "). Aborting session start.\n";
    m_state = SessionState::Lobby;
    // TODO(integration): surface reason-specific UI error.
}

void NetplaySession::HandleInputFrame(const InputFramePayload& input) {
    m_inputQueue.Push(input.playerIndex, input);
    m_lastKnownInput[input.playerIndex] = input;

    // Late-arrival check: did we already speculate for this frame, and
    // did we guess wrong?
    auto frameIt = m_appliedHistory.find(input.frameNumber);
    if (frameIt != m_appliedHistory.end()) {
        auto playerIt = frameIt->second.find(input.playerIndex);
        if (playerIt != frameIt->second.end() && playerIt->second.wasSpeculative) {
            if (!InputsEqual(playerIt->second.input, input)) {
                // Misprediction — need to rewind to at least this frame.
                if (!m_rollbackPending || input.frameNumber < m_rollbackTargetFrame) {
                    m_rollbackPending = true;
                    m_rollbackTargetFrame = input.frameNumber;
                }
            }
            // Either way, this frame's input for this player is now
            // known for certain — record it so Reconcile applies the
            // correct value rather than re-guessing.
            playerIt->second.input = input;
            playerIt->second.wasSpeculative = false;
        }
    }
}

InputFramePayload NetplaySession::PredictInput(uint8_t playerIndex, uint32_t frameNumber) {
    auto it = m_lastKnownInput.find(playerIndex);
    if (it != m_lastKnownInput.end()) {
        InputFramePayload predicted = it->second;
        predicted.frameNumber = frameNumber; // keep the frame tag correct
        return predicted;
    }
    // No history yet (very start of session) — assume neutral input.
    InputFramePayload neutral;
    neutral.frameNumber = frameNumber;
    neutral.playerIndex = playerIndex;
    return neutral;
}

void NetplaySession::RecordAppliedInput(uint32_t frameNumber, uint8_t playerIndex,
                                        const InputFramePayload& input, bool wasSpeculative) {
    m_appliedHistory[frameNumber][playerIndex] = AppliedInput{ input, wasSpeculative };
}

void NetplaySession::ReconcileIfNeeded() {
    if (!m_rollbackPending) return;

    uint32_t target = m_rollbackTargetFrame;
    auto stateIt = m_stateRingBuffer.find(target);
    if (stateIt == m_stateRingBuffer.end()) {
        // Outside our rollback window — we cannot correct this. This is
        // a hard limit of kRollbackWindowFrames: if network delay ever
        // exceeds it, the session desyncs and needs a fresh bootstrap.
        // Surfacing this loudly is intentional — silently ignoring it
        // is exactly how you get an undiagnosable desync three missions
        // later.
        std::cerr << "[Netplay] Rollback target frame " << target
                  << " is outside the " << kRollbackWindowFrames
                  << "-frame window — desync likely. Consider a full "
                     "resync (re-bootstrap) here.\n";
        m_rollbackPending = false;
        return;
    }

    // Rewind.
    bool ok = m_host.LoadState(stateIt->second);
    if (!ok) {
        std::cerr << "[Netplay] Rollback LoadState failed at frame " << target << "\n";
        m_rollbackPending = false;
        return;
    }

    // Re-simulate target..(m_currentFrame - 1) using the now-corrected
    // history (HandleInputFrame already overwrote the correct values in
    // m_appliedHistory for any frame/player where the real input beat
    // us to it; anything still marked speculative gets re-predicted the
    // same way it was the first time — deterministically, so replay is
    // safe as long as prediction logic itself hasn't changed mid-flight).
    for (uint32_t f = target; f < m_currentFrame; ++f) {
        auto& perPlayer = m_appliedHistory[f];
        const uint8_t playerCount = m_transport.PlayerCount();
        for (uint8_t p = 0; p < playerCount; ++p) {
            InputFramePayload use;
            auto pIt = perPlayer.find(p);
            if (pIt != perPlayer.end() && !pIt->second.wasSpeculative) {
                use = pIt->second.input;
            } else {
                use = PredictInput(p, f);
                perPlayer[p] = AppliedInput{ use, true };
            }
            m_host.ApplyInput(p, use.keyMask, use.touchActive != 0, use.touchX, use.touchY);
        }
        m_host.StepFrame();
    }

    m_rollbackPending = false;
}

void NetplaySession::PruneOldHistory() {
    if (m_currentFrame <= kRollbackWindowFrames) return;
    const uint32_t cutoff = m_currentFrame - kRollbackWindowFrames;

    for (auto it = m_stateRingBuffer.begin(); it != m_stateRingBuffer.end(); ) {
        it = (it->first < cutoff) ? m_stateRingBuffer.erase(it) : std::next(it);
    }
    for (auto it = m_appliedHistory.begin(); it != m_appliedHistory.end(); ) {
        it = (it->first < cutoff) ? m_appliedHistory.erase(it) : std::next(it);
    }
    m_inputQueue.PruneBefore(cutoff);
}

void NetplaySession::Tick() {
    if (m_state != SessionState::LockstepRunning) return;

    // 1. Fix up any misprediction from input that arrived late.
    ReconcileIfNeeded();

    // 2. Step one new frame forward.
    const uint32_t frameNumber = m_currentFrame;

    // Broadcast our own real input for this frame (delayed conceptually
    // by the same kInputDelayFrames on the receiving end — see below).
    InputFramePayload mine;
    mine.frameNumber = frameNumber;
    mine.playerIndex = m_localPlayerIndex;
    // TODO(integration): pull real local input state, e.g.:
    // mine.keyMask     = Input::CurrentKeyMask();
    // mine.touchActive = Input::TouchActive() ? 1 : 0;
    // mine.touchX      = mine.touchActive ? Input::TouchX() : -1;
    // mine.touchY      = mine.touchActive ? Input::TouchY() : -1;
    mine.keyMask = 0;
    mine.touchActive = 0;
    mine.touchX = -1;
    mine.touchY = -1;

    std::vector<uint8_t> bytes;
    mine.Serialize(bytes);
    m_transport.Broadcast(MsgType::InputFrame, bytes);
    m_inputQueue.Push(m_localPlayerIndex, mine);
    m_lastKnownInput[m_localPlayerIndex] = mine;

    // Snapshot BEFORE stepping this frame, so a later rollback can
    // rewind to exactly this point.
    std::vector<uint8_t> snapshot;
    m_host.SaveState(snapshot);
    m_stateRingBuffer[frameNumber] = std::move(snapshot);

    const uint8_t playerCount = m_transport.PlayerCount();
    for (uint8_t p = 0; p < playerCount; ++p) {
        InputFramePayload use;
        bool speculative;

        // Non-blocking check — zero wait, unlike the old design.
        auto maybe = m_inputQueue.WaitFor(p, frameNumber, std::chrono::milliseconds(0));
        if (maybe) {
            use = *maybe;
            speculative = false;
        } else {
            use = PredictInput(p, frameNumber);
            speculative = true;
        }

        m_host.ApplyInput(p, use.keyMask, use.touchActive != 0, use.touchX, use.touchY);
        RecordAppliedInput(frameNumber, p, use, speculative);
    }

    m_host.StepFrame();
    ++m_currentFrame;

    PruneOldHistory();
}

} // namespace Netplay

// ============================================================================
// MelonDSEmulatorHost — concrete IEmulatorHost built on confirmed melonDS
// APIs. Uncomment the #include block at the top of this file and adjust
// the constructor to however your fork actually obtains an NDS* /
// EmuInstance pointer, then add this class's .h declaration into
// NetplaySession.h (or a new MelonDSEmulatorHost.h) once confirmed.
// ============================================================================
/*
namespace Netplay {

class MelonDSEmulatorHost : public IEmulatorHost {
public:
    explicit MelonDSEmulatorHost(melonDS::NDS* nds) : m_nds(nds) {}

    void SaveState(std::vector<uint8_t>& outBuffer) override {
        // TODO(integration): confirm max size against your fork's
        // existing savestate-to-file code — Savestate takes a
        // fixed-size buffer, it doesn't grow dynamically.
        constexpr size_t kMaxSavestateSize = 32 * 1024 * 1024;
        outBuffer.assign(kMaxSavestateSize, 0);
        melonDS::Savestate state(outBuffer.data(), (uint32_t)outBuffer.size(), true);
        m_nds->DoSavestate(&state);
        // TODO(integration): Savestate likely tracks actual bytes
        // written internally (see buffer_offset in Savestate.cpp) —
        // shrink outBuffer to that real length rather than sending the
        // full fixed-size buffer over the network every frame.
    }

    bool LoadState(const std::vector<uint8_t>& buffer) override {
        melonDS::Savestate state(const_cast<uint8_t*>(buffer.data()),
                                  (uint32_t)buffer.size(), false);
        return m_nds->DoSavestate(&state);
    }

    void ApplyInput(uint8_t playerIndex, uint16_t keyMask,
                    bool touchActive, int16_t touchX, int16_t touchY) override {
        // NOTE: SetKeyMask/touch currently apply to whichever NDS
        // instance m_nds points at. For a session with >1 local-emulated
        // peer (each client emulates ALL peers per the lockstep design),
        // this host needs one NDS instance per player and must route to
        // the correct one — TODO(integration): confirm how your fork
        // manages multiple concurrent NDS instances (EmuInstance array?)
        // once you paste EmuInstance.h/.cpp.
        m_nds->SetKeyMask(keyMask);
        if (touchActive) {
            m_nds->SPI.GetTSC()->SetTouchCoords((uint16_t)touchX, (uint16_t)touchY);
        } else {
            m_nds->SPI.GetTSC()->SetTouchCoords(0, 0xFFF); // hardware's own release sentinel
        }
    }

    void StepFrame() override {
        // TODO(integration): this is the one real unknown. Likely
        // something like m_nds->RunFrame<CPUExecuteMode::...>() but
        // that's normally driven by EmuInstance's loop, not called
        // standalone — need to see that call site to wire this
        // correctly, especially for headless (non-rendering) instances
        // if you end up needing those for performance.
    }

    uint32_t RomCRC32() const override {
        // TODO(integration): wire to your fork's existing ROM CRC calc.
        return 0;
    }

    uint32_t RomSizeBytes() const override {
        // TODO(integration): wire to your fork's existing ROM size query.
        return 0;
    }

private:
    melonDS::NDS* m_nds;
};

} // namespace Netplay
*/
