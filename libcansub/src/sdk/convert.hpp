#ifndef CANSUB_SDK_CONVERT_HPP
#define CANSUB_SDK_CONVERT_HPP

/* Conversions between libcansub's public types and the official SDK's C types.
 *
 * Every place where an SDK type becomes a libcansub type, or back, is in this file: frames, frame
 * flags, error types, result codes, API versions and TLS host handling. The public headers in
 * include/cansub/ never include the SDK, so a change on the SDK side is absorbed here.
 *
 * Header-only and free of I/O, so test_glue.cpp can check every rule here without a device. */

#include <cstdint>
#include <cstring>
#include <string>

#include "cansub/cansub_ffi.h"
#include "cansub/frame.hpp"
#include "cansub/status.hpp"

namespace cansub::sdk
{

/* SDK result code to status_t, for results that end an operation.
 *
 *   CANSUB_RESULT_OK                   -> ok
 *   CANSUB_RESULT_CONNECTION_REJECTED  -> channel_busy      (the device refused the WebSocket:
 *                                                             another client holds the channel)
 *   CANSUB_RESULT_CONNECTION_TLS       -> tls_failed
 *   CANSUB_RESULT_CONNECTION_PROTOCOL  -> protocol_error
 *   CANSUB_RESULT_CONNECTION_LOST      -> connect_failed
 *   CANSUB_RESULT_INVALID_ARG          -> invalid_argument
 *   anything else                      -> internal_error
 *
 * Some codes mean different things depending on the call and are mapped where they occur instead,
 * in session.cpp: CANSUB_RESULT_AGAIN and CANSUB_RESULT_MEMORY (a full queue: wait, then timeout or
 * queue_full), CANSUB_RESULT_CLOSED (disconnected) and CANSUB_RESULT_INVALID_STATE (not_open).
 *
 * The status_t values with no SDK counterpart come from libcansub itself: not_open, already_open,
 * buffer_too_small, http_error, api_version_unsupported, channel_not_stopped, timeout (the SDK has
 * no timers), no_data and unsupported. */
inline status_t status_from_result(cansub_result code)
{
    switch (code)
    {
        case CANSUB_RESULT_OK:                  return status_t::ok;
        case CANSUB_RESULT_CONNECTION_REJECTED: return status_t::channel_busy;
        case CANSUB_RESULT_CONNECTION_TLS:      return status_t::tls_failed;
        case CANSUB_RESULT_CONNECTION_PROTOCOL: return status_t::protocol_error;
        case CANSUB_RESULT_CONNECTION_LOST:     return status_t::connect_failed;
        case CANSUB_RESULT_INVALID_ARG:         return status_t::invalid_argument;
        default:                                return status_t::internal_error;
    }
}

/* The device API version the SDK speaks, and the first firmware that offers it, for messages a
 * person can act on. The SDK has the final word (cansub_api_version_check); test_glue.cpp checks
 * that it still accepts this version. */
constexpr const char *required_api = "05.00";
constexpr const char *required_firmware = "02.05.00";

/* Whether the SDK speaks a device API version given unquoted, e.g. "05.00" from discovery. */
inline bool api_supported(const std::string &version)
{
    return cansub_api_version_check(version.data(), version.size(), nullptr) == CANSUB_RESULT_OK;
}

/* The SDK numbers error types BIT=1 .. CRC=5 (0 is NO_ERROR), while the wire encoding,
 * python-can-cansub and frame.hpp use BIT=0 .. CRC=4. Map by name, never by value. */
inline error_type_t error_type_from_sdk(uint8_t value)
{
    switch (value)
    {
        case CANSUB_ERROR_FRAME_TYPE_BIT_ERROR:   return error_type_t::bit;
        case CANSUB_ERROR_FRAME_TYPE_ACK_ERROR:   return error_type_t::ack;
        case CANSUB_ERROR_FRAME_TYPE_FORM_ERROR:  return error_type_t::form;
        case CANSUB_ERROR_FRAME_TYPE_STUFF_ERROR: return error_type_t::stuff;
        case CANSUB_ERROR_FRAME_TYPE_CRC_ERROR:   return error_type_t::crc;
        default:                                  return error_type_t::other;
    }
}

inline frame_t frame_from_sdk(const cansub_frame &in)
{
    frame_t out;

    out.us = in.timestamp_us;
    out.extended = (in.flags & CANSUB_FRAME_FLAG_IDE) != 0;
    out.fd = (in.flags & CANSUB_FRAME_FLAG_FDF) != 0;
    out.rtr = (in.flags & CANSUB_FRAME_FLAG_RTR) != 0;
    out.brs = (in.flags & CANSUB_FRAME_FLAG_BRS) != 0;
    out.esi = (in.flags & CANSUB_FRAME_FLAG_ESI) != 0;
    out.tx = (in.flags & CANSUB_FRAME_FLAG_TX) != 0;

    if (in.flags & CANSUB_FRAME_FLAG_ERROR)
    {
        /* The type as a named field, and also in id as python-can-cansub does.
         * No payload. */
        out.is_error = true;
        out.error = error_type_from_sdk(in.data[0]);
        out.id = (uint32_t)(uint8_t)out.error;

        return out;
    }

    out.id = in.arbitration_id;

    /* length is the byte count (8 for a classic DLC 9-15); for RTR, the requested length. */
    out.len = (in.length > sizeof(out.data)) ? (uint8_t)sizeof(out.data) : in.length;

    if (!out.rtr)
    {
        memcpy(out.data, in.data, out.len);
    }

    return out;
}

/* Builds a frame to send. Returns false for a frame that cannot be sent: an error frame, or a
 * payload length that is not a valid CAN (FD) length. The tx flag is receive-only and ignored. */
inline bool frame_to_sdk(const frame_t &in, cansub_frame &out)
{
    memset(&out, 0, sizeof(out));

    if (in.is_error)
    {
        return false;
    }

    if (!in.fd && in.len > 8)
    {
        return false;
    }

    const uint32_t dlc = cansub_frame_nob_to_dlc(in.len);

    if (cansub_frame_dlc_to_nob(dlc, in.fd ? 1 : 0) != in.len)
    {
        return false;
    }

    out.arbitration_id = in.id;
    out.dlc = (uint8_t)dlc;
    out.length = in.len;
    out.flags |= in.extended ? CANSUB_FRAME_FLAG_IDE : 0;
    out.flags |= in.fd ? CANSUB_FRAME_FLAG_FDF : 0;
    out.flags |= (in.rtr && !in.fd) ? CANSUB_FRAME_FLAG_RTR : 0;
    out.flags |= (in.brs && in.fd) ? CANSUB_FRAME_FLAG_BRS : 0;
    out.flags |= (in.esi && in.fd) ? CANSUB_FRAME_FLAG_ESI : 0;

    if (!(out.flags & CANSUB_FRAME_FLAG_RTR))
    {
        memcpy(out.data, in.data, in.len);
    }

    return cansub_frame_validate(&out, nullptr) == CANSUB_RESULT_OK;
}

/* True for a dotted-quad IPv4 literal such as "10.216.248.1". */
inline bool is_ipv4_literal(const std::string &host)
{
    int parts = 0;
    int digits = 0;
    int value = 0;

    for (char c : host)
    {
        if (c >= '0' && c <= '9')
        {
            value = value * 10 + (c - '0');
            digits++;

            if (digits > 3 || value > 255)
            {
                return false;
            }
        }
        else if (c == '.')
        {
            if (digits == 0)
            {
                return false;
            }

            parts++;
            digits = 0;
            value = 0;
        }
        else
        {
            return false;
        }
    }

    return digits > 0 && parts == 3;
}

/* What the TLS handshake verifies, as python-can-cansub does: a name is checked
 * against the device certificate (which names <id>-usb.local and <id>-eth.local), an IP literal
 * has no name to check, so only the CANsub root is enforced. */
inline bool accept_any_device_for(const std::string &host)
{
    return is_ipv4_literal(host);
}

} // namespace cansub::sdk

#endif
