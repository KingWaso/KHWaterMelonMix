// NetplaySession.cpp
//
// Implementation of the Stage 1 bootstrap + lockstep loop. Every place
// that needs to know about real melonDS internals is marked
// TODO(integration) with the assumed function signature — these names
// are placeholders for whatever your fork's actual ROM/Savestate/NDS/Emu
// APIs are called; grep for TODO(integration) and wire each one up.
//
// Everything else (state machine transitions, ack tracking, the input
// queue) is self-contained and should not need changes.

#include "NetplaySession.h"

#include <cassert>
#include <iostream>

// TODO(integration): replace these includes with your fork's real headers.
// #include "ROM.h"
// #include "Savestate.h"
// #include "NDS.h"
// #include "Emu.h"
// #include "Input.h"

namespace Netplay {

NetplaySession::NetplaySession(ITransport& transport)
    : m_transport(transport) {
    m_localPlayerIndex = transport.LocalPlayerIndex();
}

// ---------------------------------------------------------------------
// Host: begin bootstrap
// ---------------------------------------------------------------------
void NetplaySession::Host_BeginBootstrap() {
    // TODO(integration): pause the emulator core before snapshotting.
    // Emu::Pause();

    RomInfoPayload info;
    // TODO(integration): compute these from the loaded ROM.
    // info.romCRC32     = ROM::CurrentCRC32();
    // info.romSizeBytes = ROM::CurrentSizeBytes();
    info.romCRC32     = 0; // placeholder
    info.romSizeBytes = 0; // placeholder
    info.playerCount  = m_transport.PlayerCount();

    // TODO(integration): serialize the current emulator state into a byte
    // buffer. Something like:
    //   std::vector<uint8_t> stateBlob;
    //   Savestate state(&stateBlob, /*write=*/true);
    //   Emu::CurrentInstance()->DoSavestate(&state);
    std::vector<uint8_t> stateBlob; // placeholder, empty

    m_acksReceived.assign(info.playerCount, false);
    // The host counts itself as already "acked" — it doesn't need to
    // receive its own blob back.
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

    // NOTE: this function doesn't block. Call a periodic tick (e.g. from
    // your existing UI/main loop timer) that checks whether
    // m_bootstrapDeadline has passed and, if so, resends RomInfo +
    // InitStateBlob only to peers whose m_acksReceived[i] is still false.
    // That resend loop isn't included here since it just re-runs the
    // per-peer send block above filtered by m_acksReceived — wire it into
    // whatever periodic timer your UI already has for the lobby screen.
}

// ---------------------------------------------------------------------
// Message dispatch
// ---------------------------------------------------------------------
void NetplaySession::OnMessageReceived(MsgType type, const uint8_t* payload, size_t len) {
    switch (type) {
        case MsgType::RomInfo: {
            RomInfoPayload info;
            if (RomInfoPayload::Deserialize(payload, len, info))
                HandleRomInfo(info);
            break;
        }
        case MsgType::InitStateBlob: {
            HandleInitStateBlob(payload, len);
            break;
        }
        case MsgType::InitStateAck: {
            if (len >= 1) HandleInitStateAck(payload[0]);
            break;
        }
        case MsgType::InitStateNack: {
            if (len >= 2) HandleInitStateNack(payload[0], static_cast<NackReason>(payload[1]));
            break;
        }
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
            // Stage 3 / existing-flow message types — not handled here.
            break;
    }
}

void NetplaySession::HandleRomInfo(const RomInfoPayload& info) {
    // TODO(integration): compare against the locally loaded ROM.
    // uint32_t localCrc = ROM::CurrentCRC32();
    uint32_t localCrc = info.romCRC32; // placeholder: always "matches"

    if (localCrc != info.romCRC32) {
        std::vector<uint8_t> nackPayload{
            m_localPlayerIndex,
            static_cast<uint8_t>(NackReason::CrcMismatch)
        };
        m_transport.Broadcast(MsgType::InitStateNack, nackPayload);
        // TODO(integration): surface a user-facing error, e.g.
        // UI::ShowError("Players are running different ROM versions.");
        return;
    }

    m_localPlayerIndex = info.yourPlayerIndex;
    // Wait for InitStateBlob next.
}

void NetplaySession::HandleInitStateBlob(const uint8_t* data, size_t len) {
    // TODO(integration): load the savestate from `data`/`len` into the
    // local emulator instance. Something like:
    //   Savestate state(const_cast<uint8_t*>(data), len, /*write=*/false);
    //   bool ok = Emu::CurrentInstance()->DoSavestate(&state);
    bool ok = true; // placeholder

    std::vector<uint8_t> ackPayload{ m_localPlayerIndex };
    if (ok) {
        m_transport.Broadcast(MsgType::InitStateAck, ackPayload);
        m_currentFrame = 0;
        m_inputQueue.Reset();
        m_state = SessionState::LockstepRunning;
        // TODO(integration): Emu::Resume();
    } else {
        std::vector<uint8_t> nackPayload{
            m_localPlayerIndex,
            static_cast<uint8_t>(NackReason::LoadFailed)
        };
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
        m_state = SessionState::LockstepRunning;
        // TODO(integration): Emu::Resume();
    }
}

void NetplaySession::HandleInitStateNack(uint8_t fromPlayer, NackReason reason) {
    // Stage 1: abort the whole session start rather than silently
    // continuing with a mismatched peer. Surface this to the UI —
    // don't let bootstrap "half succeed."
    std::cerr << "[Netplay] Player " << static_cast<int>(fromPlayer)
              << " failed bootstrap (reason=" << static_cast<int>(reason)
              << "). Aborting session start.\n";
    m_state = SessionState::Lobby;
    // TODO(integration): UI::ShowError(...) with a reason-specific message,
    // and Emu::Resume() so the host isn't left paused indefinitely.
}

void NetplaySession::HandleInputFrame(const InputFramePayload& input) {
    m_inputQueue.Push(input.playerIndex, input);
}

// ---------------------------------------------------------------------
// Steady-state lockstep loop
// ---------------------------------------------------------------------
void NetplaySession::PreFrameStep(uint32_t frameNumber) {
    if (m_state != SessionState::LockstepRunning) return;

    InputFramePayload mine;
    mine.frameNumber = frameNumber;
    mine.playerIndex = m_localPlayerIndex;

    // TODO(integration): pull real local input state.
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

    const uint32_t targetFrame = (frameNumber >= kInputDelayFrames)
        ? frameNumber - kInputDelayFrames : 0;

    const uint8_t playerCount = m_transport.PlayerCount();
    for (uint8_t p = 0; p < playerCount; ++p) {
        // Stage 1: block-with-retry rather than giving up. If this
        // threshold is repeatedly exceeded, that's a signal to check
        // your relay's latency/loss, not something to silently paper
        // over by proceeding without the input.
        auto result = m_inputQueue.WaitFor(p, targetFrame, kInputWaitWarnThreshold);
        while (!result) {
            std::cerr << "[Netplay] Still waiting on player " << static_cast<int>(p)
                      << " input for frame " << targetFrame << "...\n";
            result = m_inputQueue.WaitFor(p, targetFrame, kInputWaitWarnThreshold);
        }

        // TODO(integration): apply to the emulator core, e.g.
        // NDS::ApplyNetplayInput(p, result->keyMask, result->touchActive,
        //                         result->touchX, result->touchY);
    }

    m_inputQueue.PruneBefore(targetFrame > 60 ? targetFrame - 60 : 0);
    m_currentFrame = frameNumber;
}

} // namespace Netplay
