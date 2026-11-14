#ifndef CANSUB_RP1210_J1939_ID_HPP
#define CANSUB_RP1210_J1939_ID_HPP

/* The J1939 identifier layer: 29-bit CAN identifier <-> (PGN, priority, SA, DA).
 *
 * Pure and header-only on purpose. This is the most bug-prone part of a J1939 implementation and
 * the only part that can be tested exhaustively without hardware, so it depends on nothing: no
 * cansub::bus, no client_t, no Windows. tests/test_j1939.cpp links it on its own. */

#include <cstdint>

namespace rp1210
{
namespace j1939
{

/* J1939 addresses. 254 is the null address a node uses before it owns one; 255 is the broadcast. */
constexpr uint8_t address_null = 0xFE;
constexpr uint8_t address_global = 0xFF;

/* PDU format threshold. Below it the PS byte is a destination address (PDU1, peer to peer); at or
 * above it the PS byte is a group extension and the message is a broadcast (PDU2). */
constexpr uint8_t pdu2_threshold = 0xF0;

/* The four PGNs the driver acts on itself rather than just forwarding. All are PDU1, so the value
 * carries a zero PS byte; the real destination lives in the identifier. */
constexpr uint32_t pgn_request = 0x00EA00;       /* Request, PF 0xEA */
constexpr uint32_t pgn_tp_dt = 0x00EB00;         /* Transport protocol data transfer, PF 0xEB */
constexpr uint32_t pgn_tp_cm = 0x00EC00;         /* Transport protocol connection management, PF 0xEC */
constexpr uint32_t pgn_address_claim = 0x00EE00; /* Address Claimed, PF 0xEE */

/* An RP1210 J1939 message header, which is what the identifier decodes to. */
struct addr_t
{
    uint32_t pgn = 0;  /* 18 bits: EDP, DP, PF, and PS for PDU2 only */
    uint8_t priority = 0;
    uint8_t sa = 0;
    uint8_t da = address_global;
};

inline uint8_t pdu_format(uint32_t pgn)
{
    return (uint8_t)((pgn >> 8) & 0xFF);
}

inline bool is_pdu1(uint32_t pgn)
{
    return pdu_format(pgn) < pdu2_threshold;
}

inline bool is_pdu2(uint32_t pgn)
{
    return !is_pdu1(pgn);
}

/* Normalises a PGN the way RP1210 reports it: a PDU1 PGN always carries a zero PS byte, because the
 * destination travels in the message's own DA field instead. */
inline uint32_t normalise_pgn(uint32_t pgn)
{
    pgn &= 0x3FFFF;

    return is_pdu1(pgn) ? (pgn & 0x3FF00) : pgn;
}

/* Splits a 29-bit identifier.
 *
 * Both page bits are kept. python-can-j1939 models only the Data Page and silently drops the
 * Extended Data Page, which is fine for plain J1939 but wrong here: RP1210E section 17.6 requires
 * that ISO15765 traffic riding a J1939 bus on those pages still reaches the application, so the PGN
 * is 18 bits and not 17. Do not "simplify" this back. */
inline addr_t decode(uint32_t id)
{
    const uint8_t pf = (uint8_t)((id >> 16) & 0xFF);
    const uint32_t pages = (id >> 24) & 0x03; /* bit 24 DP, bit 25 EDP */
    const uint8_t ps = (uint8_t)((id >> 8) & 0xFF);

    addr_t out;

    out.priority = (uint8_t)((id >> 26) & 0x07);
    out.sa = (uint8_t)(id & 0xFF);

    if (pf < pdu2_threshold)
    {
        /* PDU1: the PS byte is the destination, and the PGN keeps a zero there. */
        out.pgn = (pages << 16) | ((uint32_t)pf << 8);
        out.da = ps;
    }
    else
    {
        /* PDU2: the PS byte belongs to the PGN, and the destination is everyone. */
        out.pgn = (pages << 16) | ((uint32_t)pf << 8) | ps;
        out.da = address_global;
    }

    return out;
}

/* Builds a 29-bit identifier. For PDU1 the destination goes in the PS byte; for PDU2 the PGN's own
 * PS byte does, and the destination is ignored because the message is a broadcast either way. */
inline uint32_t encode(uint32_t pgn, uint8_t priority, uint8_t sa, uint8_t da)
{
    pgn &= 0x3FFFF;

    const uint8_t pf = pdu_format(pgn);
    const uint32_t pages = (pgn >> 16) & 0x03;
    const uint8_t ps = (pf < pdu2_threshold) ? da : (uint8_t)(pgn & 0xFF);

    return ((uint32_t)(priority & 0x07) << 26) | (pages << 24) | ((uint32_t)pf << 16)
           | ((uint32_t)ps << 8) | sa;
}

inline uint32_t encode(const addr_t &a)
{
    return encode(a.pgn, a.priority, a.sa, a.da);
}

/* The 8-byte NAME travels little-endian: byte 0 holds the low bits of the Identity Number and byte 7
 * the Arbitrary Address Capable bit. Contention is decided on the whole NAME as one 64-bit unsigned
 * number, and the lower value wins.
 *
 * Two independent implementations agree on this order (python-can-j1939's Name.bytes, and
 * dfieschko/RP1210's generateNetMgmtName), so the eight bytes an application hands to
 * Protect_J1939_Address go on the wire unchanged. The trap: that same RP1210 library's other path,
 * sanitize_msg_param(int, 8), is big-endian, which is why its unit test looks byte-reversed. */
inline uint64_t name_value(const uint8_t name[8])
{
    uint64_t value = 0;

    for (int i = 7; i >= 0; i--)
    {
        value = (value << 8) | name[i];
    }

    return value;
}

inline bool name_is_arbitrary_address_capable(const uint8_t name[8])
{
    return (name[7] & 0x80) != 0;
}

/* J1939-81: only the arbitrary/self-configurable range has to sit out the 250 ms contention window
 * before its address may be used. Preferred addresses (0..127) and the service tool range
 * (248..253) are usable as soon as the claim has been sent. A diagnostic tool claiming 249 therefore
 * gets an immediate answer. */
inline bool address_needs_veto_wait(uint8_t address)
{
    return address > 127 && address < 248;
}

inline bool address_is_claimable(uint8_t address)
{
    return address < address_null;
}

} // namespace j1939
} // namespace rp1210

#endif
