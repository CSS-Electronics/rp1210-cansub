#ifndef CANSUB_BUS_HPP
#define CANSUB_BUS_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cansub/bit_timing.hpp"
#include "cansub/frame.hpp"
#include "cansub/status.hpp"

namespace cansub
{

/* A receive filter, as the device's hardware accepts it. Without any filter installed the device
 * passes nothing, so open() installs an accept-all pair unless filters are supplied. */
struct filter_t
{
    bool extended = false;
    bool range = false;

    /* range == false: f1 is the id, f2 the mask. range == true: f1..f2 inclusive. */
    uint32_t f1 = 0;
    uint32_t f2 = 0;

    static filter_t accept_all(bool extended)
    {
        return filter_t{extended, false, 0, 0};
    }

    static filter_t id(uint32_t id, bool extended = false)
    {
        return filter_t{extended, false, id, extended ? 0x1FFFFFFFu : 0x7FFu};
    }
};

struct config_t
{
    uint32_t bitrate = 250000;

    /* Non-zero switches the channel to CAN FD. */
    uint32_t data_bitrate = 0;

    bool listen_only = false;
    bool auto_reset = true;

    /* Deliver bus error frames. Off by default, as on the device. */
    bool error_frames = false;

    /* Echo transmitted frames back with tx set, carrying the actual transmission timestamp. */
    bool receive_own_messages = false;

    /* Set the device clock to host time on open when they differ by more than this many seconds.
     * The device timestamps frames, so a wrong clock means wrong timestamps. */
    double max_clock_skew = 1.0;

    std::vector<filter_t> filters;
};

/* One CAN channel on one device. Not copyable; move it or hold it by pointer.
 *
 * send() and recv() are safe to call from different threads. open() and close() are not. */
class bus
{
public:
    bus();
    ~bus();

    bus(const bus &) = delete;
    bus &operator=(const bus &) = delete;

    /* host is the device name, e.g. "1a8788d1-usb.local". channel is 1-based, as in
     * python-can-cansub. Only one client may hold a channel open at a time. */
    status_t open(const std::string &host, int channel, const config_t &config = {});
    void close();

    bool is_open() const;

    status_t send(const frame_t &frame);

    /* Waits up to timeout_ms for a frame. timeout_ms < 0 waits indefinitely; 0 polls. */
    status_t recv(frame_t &frame, int timeout_ms = -1);

    /* Replaces the installed hardware filters. An empty list means accept everything, which is
     * python-can's convention and what python-can-cansub does. */
    status_t set_filters(const std::vector<filter_t> &filters);

    /* Installs exactly the filters given, with no accept-all substitution: an empty list leaves the
     * device passing nothing. Same name and meaning as python-can-cansub's set_hw_filters. Use it
     * when the caller must opt in to traffic, as an RP1210 client does. */
    status_t set_hw_filters(const std::vector<filter_t> &filters);

    /* Discards everything queued locally. Frames already on the wire are unaffected. */
    void flush();

    const std::string &device_id() const;
    const std::string &firmware_version() const;
    const std::string &api_version() const;
    bit_timing_t timing() const;

    /* Detail for the last failure, for logging. */
    std::string last_error() const;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace cansub

#endif
