#include "session.hpp"

#include "convert.hpp"

namespace cansub::sdk
{

namespace
{

using clock_t_ = std::chrono::steady_clock;

/* Timeouts the SDK leaves to the glue (see "Timeouts" in cansub_ffi.h). */
constexpr int connect_timeout_ms = 3000;  /* TCP connect, and for control the TLS handshake */
constexpr int request_timeout_ms = 5000;  /* a REST request in flight */
constexpr int open_timeout_ms = 5000;     /* data stream: connect + TLS + WebSocket upgrade */
constexpr int close_timeout_ms = 1000;    /* graceful close, before forcing it */
constexpr int abort_timeout_ms = 1000;    /* forced close, before giving up */
constexpr int send_timeout_ms = 1000;     /* a full transmit queue */

constexpr int poll_interval_ms = 10;
constexpr size_t io_chunk = 16 * 1024;
constexpr size_t in_limit = 256 * 1024; /* stop reading while this much awaits the SDK */

const cansub_stream stream_ids[2] = {CANSUB_STREAM_DATA, CANSUB_STREAM_CONTROL};

std::string result_name(cansub_result code)
{
    char name[64] = {};

    if (cansub_result_name(code, name, sizeof(name)) > 0)
    {
        return name;
    }

    return std::to_string(code);
}

bool state_in(cansub_state state, std::initializer_list<cansub_state_code> states)
{
    for (cansub_state_code s : states)
    {
        if (state == (cansub_state)s)
        {
            return true;
        }
    }

    return false;
}

} // namespace

session::~session()
{
    close();
}

status_t session::start(const std::string &host, int channel, frame_handler on_frame,
                        end_handler on_end, std::string &error)
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (ctx_ != nullptr)
    {
        return status_t::already_open;
    }

    if (!resolve_ipv4(host, 443, address_, error))
    {
        return status_t::connect_failed;
    }

    if (cansub_library_init() != CANSUB_RESULT_OK)
    {
        error = "CANsub SDK initialisation failed";

        return status_t::internal_error;
    }

    library_held_ = true;
    host_ = host;

    cansub_context_options options = {};
    options.struct_size = sizeof(options);
    options.host = host_.c_str();
    options.channel = (uint32_t)channel;
    options.accept_any_device = accept_any_device_for(host) ? 1 : 0;

    const cansub_result created = cansub_context_create(&ctx_, &options);

    if (created != CANSUB_RESULT_OK)
    {
        cansub_options_faults faults = 0;

        cansub_context_options_validate(&options, &faults);
        error = "CANsub SDK context: " + result_name(created) + " (option faults 0x"
                + std::to_string(faults) + ")";
        ctx_ = nullptr;
        cansub_library_deinit();
        library_held_ = false;

        return (created == CANSUB_RESULT_INVALID_ARG) ? status_t::invalid_argument
                                                      : status_t::internal_error;
    }

    on_frame_ = std::move(on_frame);
    on_end_ = std::move(on_end);
    stop_ = false;
    io_ = std::thread([this] { io_loop(); });

    return status_t::ok;
}

status_t session::request(const char *method, const std::string &path, const std::string &body,
                          int &http_status, std::string &response, std::string &error)
{
    std::unique_lock<std::mutex> lock(mutex_);

    http_status = 0;
    response.clear();

    if (ctx_ == nullptr)
    {
        return status_t::not_open;
    }

    const uint64_t tag = next_tag_++;
    const auto deadline = clock_t_::now()
                          + std::chrono::milliseconds(connect_timeout_ms + request_timeout_ms + 2000);

    for (;;)
    {
        const cansub_result queued = cansub_request_send(
            ctx_, tag, method, path.c_str(), body.empty() ? nullptr : "application/json",
            body.empty() ? nullptr : body.data(), body.size());

        if (queued == CANSUB_RESULT_OK)
        {
            break;
        }

        if (queued == CANSUB_RESULT_AGAIN || queued == CANSUB_RESULT_MEMORY)
        {
            if (clock_t_::now() >= deadline)
            {
                error = std::string(method) + " " + path + ": request queue full";

                return status_t::timeout;
            }

            changed_.wait_for(lock, std::chrono::milliseconds(5));

            continue;
        }

        error = std::string(method) + " " + path + ": " + result_name(queued);

        return (queued == CANSUB_RESULT_CLOSED) ? status_t::disconnected
                                                 : status_t::invalid_argument;
    }

    outstanding_++;
    pump();

    if (!changed_.wait_until(lock, deadline, [&] { return responses_.count(tag) != 0; }))
    {
        abandoned_.insert(tag);
        error = std::string(method) + " " + path + ": no response";

        return status_t::timeout;
    }

    const response_t reply = std::move(responses_[tag]);

    responses_.erase(tag);

    if (reply.result != CANSUB_RESULT_OK)
    {
        error = std::string(method) + " " + path + ": " + result_name(reply.result);

        return status_from_result(reply.result);
    }

    http_status = (int)reply.http_status;
    response = reply.body;

    return status_t::ok;
}

status_t session::open(std::string &error)
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (ctx_ == nullptr)
    {
        return status_t::not_open;
    }

    const cansub_result started = cansub_open(ctx_);

    if (started != CANSUB_RESULT_OK)
    {
        error = "open: " + result_name(started);

        return status_from_result(started);
    }

    pump();

    const bool settled = wait_state(lock, open_timeout_ms, [](cansub_state state) {
        return !state_in(state, {CANSUB_STATE_INITIALIZED, CANSUB_STATE_OPENING});
    });

    if (!settled)
    {
        /* Ending the transport records CONNECTION_LOST and finishes the data stream. */
        end_transport(CANSUB_STREAM_DATA);
        pump();
        error = "websocket did not open within " + std::to_string(open_timeout_ms) + " ms";

        return status_t::timeout;
    }

    if (cansub_get_state(ctx_) == CANSUB_STATE_OPEN)
    {
        was_open_ = true;

        return status_t::ok;
    }

    const cansub_result recorded = cansub_error(ctx_, nullptr, 0, nullptr);

    error = "websocket: " + sdk_error();

    return (recorded == CANSUB_RESULT_OK) ? status_t::connect_failed : status_from_result(recorded);
}

status_t session::send(const cansub_frame &frame)
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (ctx_ == nullptr)
    {
        return status_t::not_open;
    }

    const auto deadline = clock_t_::now() + std::chrono::milliseconds(send_timeout_ms);

    for (;;)
    {
        const cansub_result result = cansub_frame_send(ctx_, &frame);

        switch (result)
        {
            case CANSUB_RESULT_OK:
            {
                pump();

                return status_t::ok;
            }
            case CANSUB_RESULT_AGAIN:
            case CANSUB_RESULT_MEMORY:
            {
                if (cansub_get_state(ctx_) != CANSUB_STATE_OPEN && !was_open_)
                {
                    return status_t::not_open;
                }

                pump();

                if (clock_t_::now() >= deadline)
                {
                    return status_t::queue_full;
                }

                changed_.wait_for(lock, std::chrono::milliseconds(1));

                break;
            }
            case CANSUB_RESULT_CLOSED:
            {
                return status_t::disconnected;
            }
            case CANSUB_RESULT_INVALID_STATE:
            {
                return status_t::not_open;
            }
            case CANSUB_RESULT_INVALID_ARG:
            {
                return status_t::invalid_argument;
            }
            default:
            {
                return status_t::internal_error;
            }
        }
    }
}

bool session::is_open() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    return ctx_ != nullptr && cansub_get_state(ctx_) == CANSUB_STATE_OPEN;
}

void session::close()
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (ctx_ == nullptr)
    {
        return;
    }

    auto closed = [](cansub_state state) { return state == CANSUB_STATE_CLOSED; };

    if (!closed(cansub_get_state(ctx_)))
    {
        cansub_close(ctx_);
        pump();

        if (!wait_state(lock, close_timeout_ms, closed))
        {
            /* The device only confirms a close once its queued frames are on the bus, which never
             * happens on a bus where nothing acknowledges them. Make it discard them. */
            cansub_close_force(ctx_);
            pump();

            if (!wait_state(lock, abort_timeout_ms, closed))
            {
                end_transport(CANSUB_STREAM_DATA);
                end_transport(CANSUB_STREAM_CONTROL);
                pump();
            }
        }
    }

    check_ended();
    stop_ = true;
    changed_.notify_all();
    lock.unlock();

    if (io_.joinable())
    {
        io_.join();
    }

    lock.lock();

    for (stream_t &stream : streams_)
    {
        stream.socket.close();
        stream.phase = phase_t::idle;
    }

    cansub_context_destroy(ctx_);
    ctx_ = nullptr;

    if (library_held_)
    {
        cansub_library_deinit();
        library_held_ = false;
    }

    responses_.clear();
    abandoned_.clear();
    outstanding_ = 0;
}

bool session::wait_state(std::unique_lock<std::mutex> &lock, int timeout_ms,
                         const std::function<bool(cansub_state)> &done)
{
    return changed_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [&] { return done(cansub_get_state(ctx_)); });
}

std::string session::sdk_error() const
{
    size_t size = 0;
    const cansub_result code = cansub_error(ctx_, nullptr, 0, &size);
    std::string text(size > 0 ? size : 1, '\0');

    cansub_error(ctx_, &text[0], text.size(), nullptr);
    text.resize(strlen(text.c_str()));

    return result_name(code) + (text.empty() ? "" : " (" + text + ")");
}

void session::io_loop()
{
    std::unique_lock<std::mutex> lock(mutex_);

    while (!stop_)
    {
        pump();

        WSAPOLLFD fds[2] = {};
        cansub_stream ids[2] = {};
        ULONG count = 0;

        for (cansub_stream id : stream_ids)
        {
            stream_t &stream = streams_[id];
            SHORT events = 0;

            if (stream.phase == phase_t::connecting)
            {
                events = POLLWRNORM;
            }
            else if (stream.phase == phase_t::connected)
            {
                if (!stream.peer_eof && stream.in.size() - stream.in_pos < in_limit)
                {
                    events |= POLLRDNORM;
                }

                if (stream.out_pos < stream.out.size())
                {
                    events |= POLLWRNORM;
                }
            }

            if (events != 0)
            {
                fds[count].fd = stream.socket.handle();
                fds[count].events = events;
                ids[count] = id;
                count++;
            }
        }

        changed_.notify_all();

        if (count == 0)
        {
            changed_.wait_for(lock, std::chrono::milliseconds(poll_interval_ms));

            continue;
        }

        lock.unlock();
        const int ready = WSAPoll(fds, count, poll_interval_ms);
        lock.lock();

        if (stop_ || ready <= 0)
        {
            continue;
        }

        for (ULONG i = 0; i < count; i++)
        {
            stream_t &stream = streams_[ids[i]];
            const SHORT revents = fds[i].revents;

            /* Another thread may have ended this transport while the lock was released. */
            if (revents == 0 || stream.socket.handle() != fds[i].fd)
            {
                continue;
            }

            if (stream.phase == phase_t::connecting)
            {
                int wsa_error = 0;

                if ((revents & POLLWRNORM) && stream.socket.finish_connect(wsa_error))
                {
                    stream.phase = phase_t::connected;

                    if (ids[i] == CANSUB_STREAM_DATA)
                    {
                        stream.deadline_set = false;
                    }

                    if (cansub_data_connected(ctx_, ids[i]) == CANSUB_RESULT_INVALID_STATE)
                    {
                        stream.socket.close();
                        stream.phase = phase_t::idle;
                    }
                }
                else
                {
                    end_transport(ids[i]);
                }

                continue;
            }

            if (revents & (POLLRDNORM | POLLHUP | POLLERR))
            {
                read_socket(ids[i]);
            }

            if (revents & POLLWRNORM)
            {
                flush_out(ids[i]);
            }
        }
    }

    changed_.notify_all();
}

void session::pump()
{
    if (ctx_ == nullptr)
    {
        return;
    }

    /* Bounded, in case the SDK keeps reporting work that does not complete. */
    for (int pass = 0; pass < 256 && step(); pass++)
    {
    }

    check_ended();
    changed_.notify_all();
}

bool session::step()
{
    bool progress = false;
    const auto now = clock_t_::now();
    cansub_ready ready = cansub_get_ready(ctx_);

    for (cansub_stream id : stream_ids)
    {
        stream_t &stream = streams_[id];
        const cansub_ready flags = cansub_ready_stream(ready, id);

        /* The library is done with this transport. */
        if (flags & CANSUB_READY_DATA_TRANSPORT_CLOSE)
        {
            if (stream.phase == phase_t::connected)
            {
                uint8_t chunk[io_chunk];

                for (;;)
                {
                    const cansub_result n = cansub_data_drain(ctx_, id, chunk, sizeof(chunk));

                    if (n <= 0)
                    {
                        break;
                    }

                    stream.out.insert(stream.out.end(), chunk, chunk + n);
                }

                /* Best effort for the last bytes, e.g. a TLS close_notify. */
                const auto give_up = clock_t_::now() + std::chrono::milliseconds(200);

                while (stream.out_pos < stream.out.size() && clock_t_::now() < give_up
                       && flush_out(id))
                {
                }
            }

            end_transport(id);
            ready = cansub_get_ready(ctx_);
            progress = true;

            continue;
        }

        /* The library wants a transport. */
        if ((flags & CANSUB_READY_DATA_TRANSPORT_OPEN) && stream.phase == phase_t::idle)
        {
            int wsa_error = 0;

            stream.out.clear();
            stream.out_pos = 0;
            stream.in.clear();
            stream.in_pos = 0;
            stream.peer_eof = false;

            if (stream.socket.start_connect(address_, wsa_error))
            {
                stream.phase = phase_t::connecting;
                stream.deadline_set = true;
                stream.deadline = now + std::chrono::milliseconds(connect_timeout_ms);
            }
            else
            {
                cansub_data_eof(ctx_, id);
            }

            ready = cansub_get_ready(ctx_);
            progress = true;

            continue;
        }

        if (stream.phase == phase_t::connecting)
        {
            if (stream.deadline_set && now >= stream.deadline)
            {
                end_transport(id);
                ready = cansub_get_ready(ctx_);
                progress = true;
            }

            continue;
        }

        if (stream.phase != phase_t::connected)
        {
            continue;
        }

        if (flush_out(id))
        {
            progress = true;
        }

        /* Drain only once the previous bytes are written, so a slow socket pushes back on the SDK
         * (and through it, on cansub_frame_send) instead of growing this buffer. */
        if ((flags & CANSUB_READY_DATA_DRAIN) && stream.out_pos == stream.out.size())
        {
            uint8_t chunk[io_chunk];
            const cansub_result n = cansub_data_drain(ctx_, id, chunk, sizeof(chunk));

            if (n > 0)
            {
                stream.out.assign(chunk, chunk + n);
                stream.out_pos = 0;
                flush_out(id);
                progress = true;
            }
        }

        if ((flags & CANSUB_READY_DATA_SINK) && stream.in_pos < stream.in.size())
        {
            const cansub_result n = cansub_data_sink(ctx_, id, stream.in.data() + stream.in_pos,
                                                     stream.in.size() - stream.in_pos);

            if (n > 0)
            {
                stream.in_pos += (size_t)n;
                progress = true;
            }
            else if (n == CANSUB_RESULT_CLOSED)
            {
                stream.in_pos = stream.in.size();
            }

            if (stream.in_pos == stream.in.size())
            {
                stream.in.clear();
                stream.in_pos = 0;
            }
        }

        ready = cansub_get_ready(ctx_);
    }

    if (ready & CANSUB_READY_CAN_RX)
    {
        cansub_frame frame;

        while (cansub_frame_recv(ctx_, &frame) == CANSUB_RESULT_OK)
        {
            if (on_frame_)
            {
                on_frame_(frame);
            }

            progress = true;
        }
    }

    if (ready & CANSUB_READY_CONTROL_RESPONSE)
    {
        read_responses();
        progress = true;
    }

    /* Request and control-handshake timing ("Timeouts" in cansub_ffi.h). */
    uint64_t tag = 0;
    stream_t &control = streams_[CANSUB_STREAM_CONTROL];

    if (cansub_request_in_flight(ctx_, &tag) == CANSUB_RESULT_OK)
    {
        control.deadline_set = false;

        if (!timing_request_ || tag != timed_tag_)
        {
            timing_request_ = true;
            timed_tag_ = tag;
            request_deadline_ = now + std::chrono::milliseconds(request_timeout_ms);
        }
        else if (now >= request_deadline_)
        {
            /* Ending the transport fails the request with CONNECTION_LOST. */
            timing_request_ = false;
            end_transport(CANSUB_STREAM_CONTROL);
            progress = true;
        }
    }
    else
    {
        timing_request_ = false;

        /* Only requests are timed through the handshake; a transport carrying only the abort of
         * cansub_close_force() is bounded by the close timeouts instead. */
        if (control.phase == phase_t::connected && control.deadline_set && outstanding_ > 0
            && now >= control.deadline)
        {
            end_transport(CANSUB_STREAM_CONTROL);
            progress = true;
        }
    }

    /* A transport the remote closed: hand over everything it sent, then signal the EOF. */
    ready = cansub_get_ready(ctx_);

    for (cansub_stream id : stream_ids)
    {
        stream_t &stream = streams_[id];

        if (stream.phase != phase_t::connected || !stream.peer_eof)
        {
            continue;
        }

        const cansub_ready flags = cansub_ready_stream(ready, id);
        const bool can_sink = stream.in_pos < stream.in.size() && (flags & CANSUB_READY_DATA_SINK);
        const bool frames_waiting = (id == CANSUB_STREAM_DATA) && (ready & CANSUB_READY_CAN_RX);

        if (!can_sink && !frames_waiting && !(flags & CANSUB_READY_DATA_TRANSPORT_CLOSE))
        {
            end_transport(id);
            ready = cansub_get_ready(ctx_);
            progress = true;
        }
    }

    return progress;
}

bool session::flush_out(cansub_stream id)
{
    stream_t &stream = streams_[id];
    bool wrote = false;

    while (stream.phase == phase_t::connected && stream.out_pos < stream.out.size())
    {
        const int n = stream.socket.send_some(stream.out.data() + stream.out_pos,
                                              stream.out.size() - stream.out_pos);

        if (n > 0)
        {
            stream.out_pos += (size_t)n;
            wrote = true;
        }
        else
        {
            if (n < 0)
            {
                /* The connection is gone; treat it like the remote closing it. */
                stream.peer_eof = true;
                stream.out.clear();
                stream.out_pos = 0;
            }

            break;
        }
    }

    if (stream.out_pos == stream.out.size())
    {
        stream.out.clear();
        stream.out_pos = 0;
    }

    return wrote;
}

void session::read_socket(cansub_stream id)
{
    stream_t &stream = streams_[id];
    uint8_t chunk[io_chunk];

    while (!stream.peer_eof && stream.in.size() - stream.in_pos < in_limit)
    {
        const int n = stream.socket.recv_some(chunk, sizeof(chunk));

        if (n > 0)
        {
            stream.in.insert(stream.in.end(), chunk, chunk + n);
        }
        else if (n == -1)
        {
            break;
        }
        else
        {
            stream.peer_eof = true;
        }
    }
}

void session::end_transport(cansub_stream id)
{
    stream_t &stream = streams_[id];
    const cansub_ready flags = cansub_ready_stream(cansub_get_ready(ctx_), id);
    const bool known = stream.phase != phase_t::idle
                       || (flags & (CANSUB_READY_DATA_TRANSPORT_OPEN
                                    | CANSUB_READY_DATA_TRANSPORT_CLOSE));

    stream.socket.close();
    stream.phase = phase_t::idle;
    stream.deadline_set = false;
    stream.out.clear();
    stream.out_pos = 0;
    stream.in.clear();
    stream.in_pos = 0;
    stream.peer_eof = false;

    if (known)
    {
        cansub_data_eof(ctx_, id);
    }
}

void session::read_responses()
{
    for (;;)
    {
        uint64_t tag = 0;
        uint32_t http_status = 0;
        const cansub_result size = cansub_response_peek(ctx_, &tag, &http_status);

        if (size == CANSUB_RESULT_AGAIN || size == CANSUB_RESULT_CLOSED
            || size == CANSUB_RESULT_INVALID_ARG)
        {
            return;
        }

        response_t reply;

        if (size >= 0)
        {
            reply.body.resize((size_t)size);
            cansub_response_recv(ctx_, &tag, &http_status, size > 0 ? &reply.body[0] : nullptr,
                                 (size_t)size);
        }
        else
        {
            /* A failed request (CONNECTION_*); reading it consumes it. */
            cansub_response_recv(ctx_, &tag, &http_status, nullptr, 0);
            reply.result = size;
        }

        reply.http_status = http_status;

        if (outstanding_ > 0)
        {
            outstanding_--;
        }

        if (abandoned_.erase(tag) == 0)
        {
            responses_[tag] = std::move(reply);
        }
    }
}

void session::check_ended()
{
    if (ctx_ == nullptr)
    {
        return;
    }

    if (cansub_get_state(ctx_) == CANSUB_STATE_OPEN)
    {
        was_open_ = true;
    }

    if (!was_open_ || end_reported_)
    {
        return;
    }

    if (state_in(cansub_get_state(ctx_), {CANSUB_STATE_CLOSING, CANSUB_STATE_CLOSED}))
    {
        end_reported_ = true;

        if (on_end_)
        {
            on_end_();
        }
    }
}

} // namespace cansub::sdk
