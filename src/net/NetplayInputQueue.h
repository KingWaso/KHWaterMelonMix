// NetplayInputQueue.h
//
// Per-player input queue for deterministic-lockstep netplay. Stores each
// player's InputFramePayload keyed by frame number and lets the main
// emulation loop block until a given frame's input has arrived from every
// player (Stage 1 behavior — correctness over smoothness; Stage 2 can
// replace WaitFor's blocking with predict/reconcile once this is proven).

#pragma once

#include <array>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "NetplayProtocol.h"

namespace Netplay {

constexpr int kMaxPlayers = 4;

class InputQueue {
public:
    // Record a player's input for a given frame. Safe to call from the
    // network receive thread.
    void Push(uint8_t playerIndex, const InputFramePayload& input) {
        if (playerIndex >= kMaxPlayers) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_perPlayer[playerIndex][input.frameNumber] = input;
        }
        m_cv.notify_all();
    }

    // Blocks (with timeout) until playerIndex's input for frameNumber is
    // available, then returns it. If the timeout elapses, returns
    // std::nullopt — caller decides whether that means "treat as no
    // input" or "stall and retry" (Stage 1: stall and retry; see
    // NetplaySession::PreFrameStep).
    std::optional<InputFramePayload> WaitFor(uint8_t playerIndex, uint32_t frameNumber,
                                              std::chrono::milliseconds timeout) {
        if (playerIndex >= kMaxPlayers) return std::nullopt;
        std::unique_lock<std::mutex> lock(m_mutex);
        auto& map = m_perPlayer[playerIndex];
        bool ok = m_cv.wait_for(lock, timeout, [&] {
            return map.find(frameNumber) != map.end();
        });
        if (!ok) return std::nullopt;
        return map.at(frameNumber);
    }

    // Drop entries older than frameNumber for all players — call this
    // periodically (e.g. once per second) so the map doesn't grow
    // unbounded over a long mission.
    void PruneBefore(uint32_t frameNumber) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& map : m_perPlayer) {
            for (auto it = map.begin(); it != map.end(); ) {
                it = (it->first < frameNumber) ? map.erase(it) : std::next(it);
            }
        }
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& map : m_perPlayer) map.clear();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::array<std::unordered_map<uint32_t, InputFramePayload>, kMaxPlayers> m_perPlayer;
};

} // namespace Netplay
