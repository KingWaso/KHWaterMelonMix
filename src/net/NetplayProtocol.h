// NetplayProtocol.h
//
// Wire format for KHWaterMelonMix deterministic-lockstep netplay.
// Stage 1 of the relay-repurposing plan: replaces raw NiFi/WiFi frame
// relaying with (1) a session-bootstrap handshake that syncs all clients
// to an identical starting savestate, and (2) a per-frame input-exchange
// protocol that keeps every instance's local NiFi emulation in lockstep.
//
// This header has no dependencies on melonDS internals — it only defines
// the message types and payload layouts. Wiring it into the emulator core
// (ROM CRC lookup, Savestate save/load, NDS input injection) happens in
// NetplaySession.cpp — see the TODO markers there.

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace Netplay {

// ---------------------------------------------------------------------
// Message types
// ---------------------------------------------------------------------
enum class MsgType : uint8_t {
    Hello           = 0x01, // existing room-join flow, unchanged — not
                             // redefined here, just reserving the byte
    RoomJoin        = 0x02,

    RomInfo         = 0x10, // host -> each client: ROM identity + assigned
                             // player slot (payload differs per recipient
                             // in the yourPlayerIndex field)
    InitStateBlob   = 0x11, // host -> all: savestate to boot every
                             // instance from (raw bytes, no fixed struct)
    InitStateAck    = 0x12, // client -> host: loaded + CRC verified OK
    InitStateNack   = 0x13, // client -> host: failed to load / CRC mismatch

    InputFrame      = 0x20, // any peer -> all: one player's input for one
                             // emulated frame

    DesyncHash      = 0x30, // any peer -> all: periodic state hash
                             // (Stage 3 — field exists now, unused until
                             // the hashing implementation lands)

    PlayerLeft      = 0x40,
};

// ---------------------------------------------------------------------
// Wire header — every message on the netplay channel is prefixed with
// this. payloadLen is stored little-endian on the wire (see WriteHeader/
// ReadHeader below) so this struct itself should never be memcpy'd
// directly on/off a socket.
// ---------------------------------------------------------------------
struct MsgHeader {
    MsgType  type;
    uint32_t payloadLen;
};

constexpr size_t kHeaderWireSize = 1 + 4; // type byte + u32 length

inline void WriteHeader(std::vector<uint8_t>& out, MsgType type, uint32_t payloadLen) {
    out.push_back(static_cast<uint8_t>(type));
    out.push_back(static_cast<uint8_t>(payloadLen & 0xFF));
    out.push_back(static_cast<uint8_t>((payloadLen >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((payloadLen >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((payloadLen >> 24) & 0xFF));
}

// Returns false if buf doesn't contain a full header yet (caller should
// wait for more bytes — this is meant to be used against a growing
// recv buffer, same pattern your existing RelayServer framing likely
// already uses).
inline bool ReadHeader(const uint8_t* buf, size_t len, MsgHeader& outHdr) {
    if (len < kHeaderWireSize) return false;
    outHdr.type = static_cast<MsgType>(buf[0]);
    outHdr.payloadLen = static_cast<uint32_t>(buf[1])
                      | (static_cast<uint32_t>(buf[2]) << 8)
                      | (static_cast<uint32_t>(buf[3]) << 16)
                      | (static_cast<uint32_t>(buf[4]) << 24);
    return true;
}

// ---------------------------------------------------------------------
// Payload structs
//
// These are packed and written/read via the To/From helpers below rather
// than raw memcpy'd as a struct, so we don't have to worry about compiler
// padding or endianness on the actual wire — only the in-memory struct is
// convenient to work with in calling code.
// ---------------------------------------------------------------------

struct RomInfoPayload {
    uint32_t romCRC32     = 0;
    uint32_t romSizeBytes = 0;
    uint8_t  playerCount  = 0;  // 2-4
    uint8_t  yourPlayerIndex = 0; // 0-based slot for the recipient of THIS
                                  // specific message (host sends N copies,
                                  // one per peer, each with this field set
                                  // to that peer's assigned index)

    static constexpr size_t kWireSize = 4 + 4 + 1 + 1;

    void Serialize(std::vector<uint8_t>& out) const {
        auto pushU32 = [&out](uint32_t v) {
            out.push_back(static_cast<uint8_t>(v & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        };
        pushU32(romCRC32);
        pushU32(romSizeBytes);
        out.push_back(playerCount);
        out.push_back(yourPlayerIndex);
    }

    static bool Deserialize(const uint8_t* buf, size_t len, RomInfoPayload& out) {
        if (len < kWireSize) return false;
        auto readU32 = [&buf](size_t off) -> uint32_t {
            return static_cast<uint32_t>(buf[off])
                 | (static_cast<uint32_t>(buf[off + 1]) << 8)
                 | (static_cast<uint32_t>(buf[off + 2]) << 16)
                 | (static_cast<uint32_t>(buf[off + 3]) << 24);
        };
        out.romCRC32     = readU32(0);
        out.romSizeBytes = readU32(4);
        out.playerCount  = buf[8];
        out.yourPlayerIndex = buf[9];
        return true;
    }
};

struct InputFramePayload {
    uint32_t frameNumber   = 0;
    uint8_t  playerIndex   = 0;
    uint16_t keyMask       = 0;   // matches NDS key-input bitmask layout
    int16_t  touchX        = -1;  // -1 == not touching
    int16_t  touchY        = -1;
    uint8_t  touchActive   = 0;   // explicit flag, don't rely on X/Y sentinel alone

    static constexpr size_t kWireSize = 4 + 1 + 2 + 2 + 2 + 1;

    void Serialize(std::vector<uint8_t>& out) const {
        auto pushU32 = [&out](uint32_t v) {
            out.push_back(static_cast<uint8_t>(v & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        };
        auto pushU16 = [&out](uint16_t v) {
            out.push_back(static_cast<uint8_t>(v & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        };
        pushU32(frameNumber);
        out.push_back(playerIndex);
        pushU16(keyMask);
        pushU16(static_cast<uint16_t>(touchX));
        pushU16(static_cast<uint16_t>(touchY));
        out.push_back(touchActive);
    }

    static bool Deserialize(const uint8_t* buf, size_t len, InputFramePayload& out) {
        if (len < kWireSize) return false;
        auto readU32 = [&buf](size_t off) -> uint32_t {
            return static_cast<uint32_t>(buf[off])
                 | (static_cast<uint32_t>(buf[off + 1]) << 8)
                 | (static_cast<uint32_t>(buf[off + 2]) << 16)
                 | (static_cast<uint32_t>(buf[off + 3]) << 24);
        };
        auto readU16 = [&buf](size_t off) -> uint16_t {
            return static_cast<uint16_t>(buf[off]) | (static_cast<uint16_t>(buf[off + 1]) << 8);
        };
        out.frameNumber = readU32(0);
        out.playerIndex = buf[4];
        out.keyMask     = readU16(5);
        out.touchX      = static_cast<int16_t>(readU16(7));
        out.touchY      = static_cast<int16_t>(readU16(9));
        out.touchActive = buf[11];
        return true;
    }
};

struct DesyncHashPayload {
    uint32_t frameNumber = 0;
    uint64_t stateHash   = 0; // Stage 3 stub — wire to a real state hash later
    uint8_t  playerIndex = 0;

    static constexpr size_t kWireSize = 4 + 8 + 1;

    void Serialize(std::vector<uint8_t>& out) const {
        auto pushU32 = [&out](uint32_t v) {
            out.push_back(static_cast<uint8_t>(v & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        };
        pushU32(frameNumber);
        for (int i = 0; i < 8; ++i)
            out.push_back(static_cast<uint8_t>((stateHash >> (i * 8)) & 0xFF));
        out.push_back(playerIndex);
    }

    static bool Deserialize(const uint8_t* buf, size_t len, DesyncHashPayload& out) {
        if (len < kWireSize) return false;
        out.frameNumber = static_cast<uint32_t>(buf[0])
                        | (static_cast<uint32_t>(buf[1]) << 8)
                        | (static_cast<uint32_t>(buf[2]) << 16)
                        | (static_cast<uint32_t>(buf[3]) << 24);
        uint64_t h = 0;
        for (int i = 0; i < 8; ++i)
            h |= static_cast<uint64_t>(buf[4 + i]) << (i * 8);
        out.stateHash = h;
        out.playerIndex = buf[12];
        return true;
    }
};

} // namespace Netplay
