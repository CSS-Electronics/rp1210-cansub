#ifndef CANSUB_STATUS_HPP
#define CANSUB_STATUS_HPP

namespace cansub
{

/* Every fallible call returns one of these. No exceptions are thrown anywhere in the library: an
 * RP1210 driver is loaded into a third-party process and must not let one unwind into it. */
enum class status_t
{
    ok = 0,

    /* Caller errors */
    invalid_argument,
    not_open,
    already_open,
    buffer_too_small,

    /* Connection and protocol */
    connect_failed,
    tls_failed,
    http_error,
    api_version_unsupported,
    channel_busy,
    channel_not_stopped,

    /* Runtime */
    timeout,
    no_data,
    queue_full,
    disconnected,
    protocol_error,
    unsupported,
    internal_error,
};

const char *status_string(status_t status);

inline bool ok(status_t status)
{
    return status == status_t::ok;
}

} // namespace cansub

#endif
