#ifndef CANSUB_FRAME_HPP
#define CANSUB_FRAME_HPP

#include <cstdint>
#include <cstring>

namespace cansub
{

/* CAN error types the device reports when phy.error_frames is enabled. Values match the wire
 * encoding and python-can-cansub's CanSubErrorFrameType, so the two interfaces agree. */
enum class error_type_t : uint8_t
{
    bit = 0,
    ack = 1,
    form = 2,
    stuff = 3,
    crc = 4,
    other = 0xFF,
};

const char *error_type_string(error_type_t type);

/* A single CAN frame. Mirrors python-can's Message closely enough that anyone who knows
 * python-can-cansub can read this, with two deliberate improvements:
 *
 *   - len is the payload byte count, as python-can's Message.dlc is. The raw DLC code never
 *     surfaces here; for an RTR frame len is the requested length while no payload is carried.
 *   - an error frame gets its own is_error / error flags rather than overloading the identifier.
 *     python-can-cansub puts the type in arbitration_id because python-can offers nowhere else;
 *     id is also set here for parity, but prefer reading error. */
struct frame_t
{
    uint64_t us = 0; /* Microseconds since the Unix epoch, device-timestamped */

    uint32_t id = 0;
    bool extended = false;
    bool fd = false;
    bool rtr = false; /* Classic CAN only */
    bool brs = false; /* CAN FD only */
    bool esi = false; /* CAN FD only */
    bool tx = false;  /* Transmitted by us (a tx_ack_frames acknowledgement), not received */

    bool is_error = false;
    error_type_t error = error_type_t::other;

    uint8_t data[64] = {};
    uint8_t len = 0;

    static frame_t make(uint32_t id, const void *data, uint8_t len, bool extended = false)
    {
        frame_t frame;

        frame.id = id;
        frame.extended = extended;
        frame.len = (len > sizeof(frame.data)) ? (uint8_t)sizeof(frame.data) : len;
        memcpy(frame.data, data, frame.len);

        return frame;
    }
};

} // namespace cansub

#endif
