#ifndef CANSUB_SDK_SESSION_HPP
#define CANSUB_SDK_SESSION_HPP

/* The glue between libcansub and the official CANsub SDK (third_party/c-cpp-sdk-cansub).
 *
 * The SDK is sans-IO: it does TLS, HTTP, WebSocket and the frame codec, but owns no sockets, no
 * threads and no timers. A session supplies those for one SDK context, i.e. one CAN channel:
 *
 *   - two TCP connections, one per SDK stream (data = WebSocket with the frames, control = REST),
 *   - one I/O thread that waits on the sockets and runs the loop the SDK header describes
 *     ("Driving a context"): read the ready flags, act on each, repeat until nothing moves,
 *   - the timeouts the SDK leaves to the caller (connect, request, open, close).
 *
 * Every call on the SDK context is serialized by one mutex, as the SDK requires. A thread that
 * sends a frame or queues a request also moves the resulting bytes to the socket itself before it
 * lets go of the lock, so a send does not wait for the I/O thread to wake up.
 *
 * A session is single-use: once closed it cannot be reopened, which matches the SDK context. */

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cansub/cansub_ffi.h"
#include "cansub/status.hpp"
#include "socket.hpp"

namespace cansub::sdk
{

class session
{
public:
    /* Called with every received frame, from whichever thread is driving the context at the time,
     * with the session lock held. Must not call back into the session. */
    using frame_handler = std::function<void(const cansub_frame &)>;

    /* Called once when an open data stream ends, for whatever reason. Same rules as above. */
    using end_handler = std::function<void()>;

    session() = default;
    ~session();

    session(const session &) = delete;
    session &operator=(const session &) = delete;

    /* Creates the SDK context and starts the I/O thread. No network traffic yet. */
    status_t start(const std::string &host, int channel, frame_handler on_frame, end_handler on_end,
                   std::string &error);

    /* One REST request on the control stream, waiting for its response. ok means a response
     * arrived; http_status says what it was. */
    status_t request(const char *method, const std::string &path, const std::string &body,
                     int &http_status, std::string &response, std::string &error);

    /* Opens the data stream (the WebSocket), which starts the CAN channel on the device. */
    status_t open(std::string &error);

    status_t send(const cansub_frame &frame);

    bool is_open() const;

    /* Graceful close: lets the device send what it has queued, then forces it, then gives up.
     * Idempotent. */
    void close();

private:
    enum class phase_t
    {
        idle,
        connecting,
        connected
    };

    struct stream_t
    {
        tcp_socket socket;
        phase_t phase = phase_t::idle;

        /* Connect deadline; for the control stream also the TLS handshake (until a request is in
         * flight). */
        bool deadline_set = false;
        std::chrono::steady_clock::time_point deadline;

        std::vector<uint8_t> out; /* drained from the SDK, not yet written to the socket */
        size_t out_pos = 0;
        std::vector<uint8_t> in; /* read from the socket, not yet accepted by the SDK */
        size_t in_pos = 0;
        bool peer_eof = false;
    };

    struct response_t
    {
        cansub_result result = CANSUB_RESULT_OK;
        uint32_t http_status = 0;
        std::string body;
    };

    void io_loop();

    /* Everything below runs with mutex_ held. */
    void pump();
    bool step();
    bool flush_out(cansub_stream id);
    void read_socket(cansub_stream id);
    void end_transport(cansub_stream id);
    void read_responses();
    void check_ended();
    std::string sdk_error() const;
    bool wait_state(std::unique_lock<std::mutex> &lock, int timeout_ms,
                    const std::function<bool(cansub_state)> &done);

    mutable std::mutex mutex_;
    std::condition_variable changed_;

    cansub_ctx *ctx_ = nullptr;
    bool library_held_ = false;
    std::string host_;
    sockaddr_in address_ = {};
    bool address_ok_ = false;
    std::string address_error_;

    stream_t streams_[2];

    frame_handler on_frame_;
    end_handler on_end_;
    bool was_open_ = false;
    bool end_reported_ = false;

    uint64_t next_tag_ = 1;
    std::map<uint64_t, response_t> responses_;
    std::set<uint64_t> abandoned_;
    int outstanding_ = 0;

    bool timing_request_ = false;
    uint64_t timed_tag_ = 0;
    std::chrono::steady_clock::time_point request_deadline_;

    std::thread io_;
    bool stop_ = false;
};

} // namespace cansub::sdk

#endif
