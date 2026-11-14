#include "cansub/frame.hpp"
#include "cansub/status.hpp"

namespace cansub
{

const char *status_string(status_t status)
{
    switch (status)
    {
        case status_t::ok:                      return "ok";
        case status_t::invalid_argument:        return "invalid argument";
        case status_t::not_open:                return "bus not open";
        case status_t::already_open:            return "bus already open";
        case status_t::buffer_too_small:        return "buffer too small";
        case status_t::connect_failed:          return "connection failed";
        case status_t::tls_failed:              return "TLS handshake failed";
        case status_t::http_error:              return "HTTP request failed";
        case status_t::api_version_unsupported: return "device API version not supported";
        case status_t::channel_busy:            return "channel already has a client";
        case status_t::channel_not_stopped:     return "channel is not stopped";
        case status_t::timeout:                 return "timed out";
        case status_t::no_data:                 return "no data";
        case status_t::queue_full:              return "queue full";
        case status_t::disconnected:            return "disconnected";
        case status_t::protocol_error:          return "protocol error";
        case status_t::unsupported:             return "unsupported";
        case status_t::internal_error:          return "internal error";
    }

    return "unknown status";
}

const char *error_type_string(error_type_t type)
{
    switch (type)
    {
        case error_type_t::bit:   return "bit error";
        case error_type_t::ack:   return "ack error";
        case error_type_t::form:  return "form error";
        case error_type_t::stuff: return "stuff error";
        case error_type_t::crc:   return "crc error";
        case error_type_t::other: return "other error";
    }

    return "unknown error";
}

} // namespace cansub
