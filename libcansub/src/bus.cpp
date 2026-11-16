#include "cansub/bus.hpp"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>

#include "sdk/convert.hpp"
#include "sdk/session.hpp"
#include "transport/json.hpp"

namespace cansub
{

namespace
{

/* Bound on the receive queue. Beyond this the oldest frames are dropped, so a slow consumer cannot
 * grow memory without limit; drops are counted and reported through last_error(). */
constexpr size_t rx_queue_limit = 32768;

std::string channel_path(int channel, const char *suffix = "")
{
    return "/api/can/" + std::to_string(channel) + suffix;
}

} // namespace

/* The transport is the official SDK, driven by sdk::session. This file keeps the device-specific
 * sequence on top: which REST calls to make, in what order, and what their JSON looks like. The
 * SDK leaves all of that to its caller. */
struct bus::impl
{
    std::unique_ptr<sdk::session> link;

    std::string host;
    int channel = 0;
    config_t config;

    std::string device_id;
    std::string firmware;
    std::string api;
    bit_timing_t nominal;
    bit_timing_t data;

    std::atomic<bool> running{false};

    std::mutex rx_mutex;
    std::condition_variable rx_cv;
    std::deque<frame_t> rx_queue;
    uint64_t dropped = 0;

    std::mutex error_mutex;
    std::string error;

    void set_error(const std::string &text)
    {
        std::lock_guard<std::mutex> lock(error_mutex);

        error = text;
    }

    std::string get_error()
    {
        std::lock_guard<std::mutex> lock(error_mutex);

        return error;
    }

    /* One REST call, failing on anything but 2xx. */
    status_t api_call(const char *method, const std::string &path, const std::string &body,
                      std::string &response)
    {
        if (link == nullptr)
        {
            return status_t::not_open;
        }

        int code = 0;
        std::string detail;
        const status_t result = link->request(method, path, body, code, response, detail);

        if (!ok(result))
        {
            set_error(detail + " (" + status_string(result) + ")");

            return result;
        }

        if (code < 200 || code > 299)
        {
            set_error(std::string(method) + " " + path + ": HTTP " + std::to_string(code) + " "
                      + response);

            return status_t::http_error;
        }

        return status_t::ok;
    }

    status_t clear_collection(const char *suffix)
    {
        std::string body;

        if (!ok(api_call("GET", channel_path(channel, suffix), "", body)))
        {
            return status_t::http_error;
        }

        std::vector<int> ids;

        if (!json::parse_int_array(body, ids))
        {
            set_error(std::string("unparseable list from ") + suffix);

            return status_t::protocol_error;
        }

        for (int id : ids)
        {
            std::string ignored;

            if (!ok(api_call("DELETE", channel_path(channel, suffix) + "/" + std::to_string(id), "",
                             ignored)))
            {
                return status_t::http_error;
            }
        }

        return status_t::ok;
    }

    /* Reads the device clock and sets it from host UTC when the two differ by more than max_skew
     * seconds. A max_skew of 0 always sets it, skipping the read. API 05.00 wraps the time in an
     * object with a clock rate correction, which is left at 0. */
    status_t sync_clock(double max_skew)
    {
        const time_t host_now = time(nullptr);

        if (max_skew > 0.0)
        {
            std::string body;

            if (!ok(api_call("GET", "/api/time", "", body)))
            {
                return status_t::http_error;
            }

            std::string stamp;
            tm parts = {};
            int millis = 0;

            if (json::find_string(body, "utc", stamp)
                && sscanf(stamp.c_str(), "%d-%d-%dT%d:%d:%d.%dZ", &parts.tm_year, &parts.tm_mon,
                          &parts.tm_mday, &parts.tm_hour, &parts.tm_min, &parts.tm_sec, &millis) == 7)
            {
                parts.tm_year -= 1900;
                parts.tm_mon -= 1;

                const time_t device_now = _mkgmtime(&parts);

                if (device_now != (time_t)-1 && fabs(difftime(host_now, device_now)) <= max_skew)
                {
                    return status_t::ok;
                }
            }
        }

        tm utc = {};

        if (gmtime_s(&utc, &host_now) != 0)
        {
            set_error("cannot read host UTC clock");

            return status_t::internal_error;
        }

        char stamp[40];

        snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02d.000Z", utc.tm_year + 1900,
                 utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);

        json::builder body;

        body.object_begin().field("utc", std::string(stamp)).field("rate_ppb", (int64_t)0).object_end();

        std::string ignored;

        return api_call("PUT", "/api/time", body.str(), ignored);
    }

    status_t install_filters(const std::vector<filter_t> &filters, bool accept_all_if_empty)
    {
        if (!ok(clear_collection("/filter")))
        {
            return status_t::http_error;
        }

        /* The device passes nothing while no filter is installed. For set_filters an empty request
         * therefore means accept-all, which is python-can's convention; set_hw_filters passes
         * accept_all_if_empty false to get the device's literal behaviour instead. */
        std::vector<filter_t> effective = filters;

        if (effective.empty() && accept_all_if_empty)
        {
            effective.push_back(filter_t::accept_all(false));
            effective.push_back(filter_t::accept_all(true));
        }

        for (const filter_t &f : effective)
        {
            json::builder json_body;

            json_body.object_begin()
                .field("is_extended", f.extended)
                .field("is_range", f.range)
                .field("f1", (int64_t)f.f1)
                .field("f2", (int64_t)f.f2)
                .object_end();

            std::string ignored;

            if (!ok(api_call("POST", channel_path(channel, "/filter"), json_body.str(), ignored)))
            {
                return status_t::http_error;
            }
        }

        return status_t::ok;
    }

    /* Called by the session, with its lock held, for every received frame. */
    void on_frame(const cansub_frame &raw)
    {
        const frame_t frame = sdk::frame_from_sdk(raw);

        {
            std::lock_guard<std::mutex> lock(rx_mutex);

            if (rx_queue.size() >= rx_queue_limit)
            {
                rx_queue.pop_front();
                dropped++;
            }

            rx_queue.push_back(frame);
        }

        rx_cv.notify_all();
    }

    /* Called by the session when the open WebSocket ends, e.g. the device went away. */
    void on_end()
    {
        if (running.exchange(false))
        {
            set_error("websocket closed by the device or the connection was lost");
        }

        {
            std::lock_guard<std::mutex> lock(rx_mutex);
        }

        rx_cv.notify_all();
    }
};

bus::bus() : impl_(new impl())
{
}

bus::~bus()
{
    close();
}

status_t bus::open(const std::string &host, int channel, const config_t &config)
{
    impl &self = *impl_;

    if (self.link != nullptr)
    {
        return status_t::already_open;
    }

    if (channel < 1)
    {
        return status_t::invalid_argument;
    }

    /* The phy endpoint requires a data phase even for classic CAN, so a channel without a data
     * bitrate gets 1 Mbit/s at the default sample point, as python-can-cansub does. */
    const bool fd = config.data_bitrate != 0;
    const uint32_t data_bitrate = fd ? config.data_bitrate : 1000000;
    const uint32_t data_sample_point =
        fd ? config.data_sample_point : bit_timing_t::default_sample_point;

    if (!bit_timing_t::calculate(config.bitrate, config.sample_point, data_bitrate,
                                 data_sample_point, self.nominal, self.data))
    {
        self.set_error("unsupported bit timing: " + std::to_string(config.bitrate) + " bit/s at "
                       + std::to_string(config.sample_point) + "/100 %, data "
                       + std::to_string(data_bitrate) + " bit/s at "
                       + std::to_string(data_sample_point) + "/100 %");

        return status_t::invalid_argument;
    }

    self.host = host;
    self.channel = channel;
    self.config = config;
    self.dropped = 0;

    {
        std::lock_guard<std::mutex> lock(self.rx_mutex);

        self.rx_queue.clear();
    }

    self.link.reset(new sdk::session());

    std::string detail;
    status_t result = self.link->start(
        host, channel, [&self](const cansub_frame &frame) { self.on_frame(frame); },
        [&self] { self.on_end(); }, detail);

    if (!ok(result))
    {
        self.set_error("connect to " + host + ": " + detail);
        self.link.reset();

        /* A device that cannot be reached fails like its first request. */
        return (result == status_t::invalid_argument) ? result : status_t::http_error;
    }

    std::string body;

    /* Version first: it doubles as the connection test, and the SDK requires it. */
    if (!ok(self.api_call("GET", "/api/version", "", body)))
    {
        close();

        return status_t::http_error;
    }

    if (!json::parse_string(body, self.api) || !sdk::api_supported(self.api))
    {
        /* Name the firmware, not just the API version: an API number is not something anyone can
         * act on, and the firmware release is what they would actually change. */
        self.set_error("device API version " + self.api + " not supported (need "
                       + sdk::required_api + ", i.e. firmware " + sdk::required_firmware
                       + " or later)");
        close();

        return status_t::api_version_unsupported;
    }

    if (ok(self.api_call("GET", "/api/info", "", body)))
    {
        json::find_string(body, "id", self.device_id);
        json::find_string(body, "fw_ver", self.firmware);
    }

    /* The channel must be idle, or the WebSocket upgrade fails later and less clearly.
     *
     * "state" is the CAN controller state, so anything other than "stopped" means the controller is
     * running for someone. A previous session that closed abruptly takes a moment to wind down, so
     * poll briefly rather than failing on the first look; python-can-cansub gives up immediately and
     * that is a common source of confusing back-to-back failures. */
    std::string state;
    bool stopped = false;

    for (int attempt = 0; attempt < 20 && !stopped; attempt++)
    {
        if (attempt > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!ok(self.api_call("GET", channel_path(channel), "", body)))
        {
            close();

            return status_t::http_error;
        }

        if (!json::find_string(body, "state", state))
        {
            self.set_error("no channel state in response");
            close();

            return status_t::protocol_error;
        }

        stopped = (state == "stopped");
    }

    if (!stopped)
    {
        self.set_error("channel " + std::to_string(channel) + " is " + state
                       + ", not stopped: another client is using it");
        close();

        return status_t::channel_busy;
    }

    /* The device timestamps frames, so its clock has to be right. */
    if (config.max_clock_skew >= 0.0 && !ok(self.sync_clock(config.max_clock_skew)))
    {
        close();

        return status_t::http_error;
    }

    if (!ok(self.install_filters(config.filters, true)))
    {
        close();

        return status_t::http_error;
    }

    if (!ok(self.clear_collection("/transmit")))
    {
        close();

        return status_t::http_error;
    }

    json::builder phy;

    phy.object_begin()
        .field("listen_only", config.listen_only)
        .field("auto_reset", config.auto_reset)
        .field("error_frames", config.error_frames)
        .field("tx_ack_frames", config.receive_own_messages)
        .key("timing").object_begin()
            .field("brp", (int64_t)self.nominal.brp)
            .field("seg1", (int64_t)self.nominal.seg1)
            .field("seg2", (int64_t)self.nominal.seg2)
            .field("sjw", (int64_t)self.nominal.sjw)
        .object_end()
        .key("timing_data").object_begin()
            .field("brp", (int64_t)self.data.brp)
            .field("seg1", (int64_t)self.data.seg1)
            .field("seg2", (int64_t)self.data.seg2)
            .field("sjw", (int64_t)self.data.sjw)
        .object_end()
        .object_end();

    if (!ok(self.api_call("PUT", channel_path(channel, "/phy"), phy.str(), body)))
    {
        close();

        return status_t::http_error;
    }

    /* Opening the WebSocket starts the channel. */
    self.running.store(true);
    result = self.link->open(detail);

    if (!ok(result))
    {
        self.running.store(false);
        self.set_error("websocket upgrade: " + detail);
        close();

        return result;
    }

    return status_t::ok;
}

void bus::close()
{
    impl &self = *impl_;

    self.running.store(false);

    /* Close gracefully: the device stops the CAN channel on the WebSocket close, and without it the
     * next open finds the channel still running in whatever CAN error state it was left in. The
     * session forces the close if the device does not confirm it in time. */
    if (self.link != nullptr)
    {
        self.link->close();
        self.link.reset();
    }

    {
        std::lock_guard<std::mutex> lock(self.rx_mutex);

        self.rx_queue.clear();
    }

    self.rx_cv.notify_all();
}

bool bus::is_open() const
{
    return impl_->link != nullptr && impl_->running.load() && impl_->link->is_open();
}

status_t bus::send(const frame_t &frame)
{
    impl &self = *impl_;

    if (self.link == nullptr || !self.running.load())
    {
        return (self.link == nullptr) ? status_t::not_open : status_t::disconnected;
    }

    /* A listen-only channel never drives the bus. */
    if (self.config.listen_only)
    {
        return status_t::invalid_argument;
    }

    /* Without a data bitrate the data phase is a placeholder, so an FD frame would go out at a
     * rate nobody chose. python-can-cansub refuses this case too. */
    if (frame.fd && self.config.data_bitrate == 0)
    {
        return status_t::invalid_argument;
    }

    cansub_frame raw;

    if (!sdk::frame_to_sdk(frame, raw))
    {
        return status_t::invalid_argument;
    }

    return self.link->send(raw);
}

status_t bus::recv(frame_t &frame, int timeout_ms)
{
    impl &self = *impl_;
    std::unique_lock<std::mutex> lock(self.rx_mutex);

    auto ready = [&self] { return !self.rx_queue.empty() || !self.running.load(); };

    if (timeout_ms < 0)
    {
        self.rx_cv.wait(lock, ready);
    }
    else if (!self.rx_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready))
    {
        return status_t::timeout;
    }

    if (!self.rx_queue.empty())
    {
        frame = self.rx_queue.front();
        self.rx_queue.pop_front();

        return status_t::ok;
    }

    return self.running.load() ? status_t::timeout : status_t::disconnected;
}

status_t bus::set_filters(const std::vector<filter_t> &filters)
{
    if (impl_->link == nullptr)
    {
        return status_t::not_open;
    }

    return impl_->install_filters(filters, true);
}

status_t bus::set_hw_filters(const std::vector<filter_t> &filters)
{
    if (impl_->link == nullptr)
    {
        return status_t::not_open;
    }

    return impl_->install_filters(filters, false);
}

void bus::flush()
{
    std::lock_guard<std::mutex> lock(impl_->rx_mutex);

    impl_->rx_queue.clear();
}

const std::string &bus::device_id() const
{
    return impl_->device_id;
}

const std::string &bus::firmware_version() const
{
    return impl_->firmware;
}

const std::string &bus::api_version() const
{
    return impl_->api;
}

bit_timing_t bus::timing() const
{
    return impl_->nominal;
}

bit_timing_t bus::data_timing() const
{
    return impl_->data;
}

std::string bus::last_error() const
{
    impl &self = *impl_;
    std::string text = self.get_error();

    if (self.dropped > 0)
    {
        text += " (" + std::to_string(self.dropped) + " frame(s) dropped from a full rx queue)";
    }

    return text;
}

} // namespace cansub
