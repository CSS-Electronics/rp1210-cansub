#include "client.hpp"

#include <chrono>

namespace rp1210
{

namespace
{

client_t clients[max_clients];

} // namespace

std::mutex &table_mutex()
{
    static std::mutex mutex;

    return mutex;
}

client_t *acquire(short id)
{
    if (id < 0 || id >= max_clients)
    {
        return nullptr;
    }

    return &clients[id];
}

client_t *allocate(short &id)
{
    for (short i = 0; i < max_clients; i++)
    {
        if (!clients[i].connected && clients[i].can == nullptr)
        {
            clients[i].id = i;
            id = i;

            return &clients[i];
        }
    }

    return nullptr;
}

/* The J1939 worker. Waits for a frame or for the engine's next deadline, whichever comes first, so
 * an idle bus costs one wakeup every 50 ms and a running transfer is paced to the millisecond.
 *
 * This thread is the only caller of bus::send and bus::recv for a J1939 client, which is what makes
 * the ordering safe: a burst of data packets is never interleaved with an unrelated frame, and the
 * engine itself needs no internal locking. */
void client_t::start_worker()
{
    worker_stop = false;

    worker = std::thread([this] {
        uint32_t wait_ms = 0;

        while (!worker_stop)
        {
            cansub::frame_t frame;
            const cansub::status_t result = can->recv(frame, (int)wait_ms);

            if (worker_stop)
            {
                break;
            }

            if (result == cansub::status_t::disconnected)
            {
                break;
            }

            {
                std::lock_guard<std::mutex> lock(engine_mutex);

                if (result == cansub::status_t::ok && !frame.is_error && !frame.fd && !frame.rtr
                    && frame.extended)
                {
                    /* J1939 is 29-bit only. Standard identifiers, remote frames and anything with
                     * a flexible data rate flag are not J1939 traffic and are dropped here. */
                    j1939::raw_frame_t raw;

                    raw.id = frame.id;
                    raw.len = frame.len > 8 ? 8 : frame.len;
                    raw.us = frame.us;
                    memcpy(raw.data, frame.data, raw.len);

                    engine->feed(raw, frame.tx);
                }

                wait_ms = engine->tick();
            }

            rx_wake.notify_all();

            /* Cap the sleep so a stop request is noticed promptly even on a silent bus. */
            if (wait_ms > 50)
            {
                wait_ms = 50;
            }
        }

        /* Whatever ended the loop, wake anyone blocked in a read rather than leaving them to sit
         * out their whole timeout against a channel that is no longer being served. */
        worker_stop = true;
        rx_wake.notify_all();
    });
}

void client_t::stop_worker()
{
    if (!worker.joinable())
    {
        return;
    }

    worker_stop = true;
    engine_wake.notify_all();

    /* recv has a bounded timeout, so the thread comes back on its own within one poll interval;
     * there is no need to close the bus first, which would not be safe from here anyway. */
    worker.join();
}

void release(short id)
{
    client_t *client = acquire(id);

    if (client == nullptr)
    {
        return;
    }

    /* Stop the worker before touching the bus. It is the only user of the bus, and closing
     * underneath it would pull the object out from under a live recv. */
    client->stop_worker();
    client->engine.reset();

    /* Closing the bus stops the CAN channel, so a host application that disconnects without
     * unloading the DLL leaves the device idle for the next client. */
    if (client->can != nullptr)
    {
        client->can->close();
        client->can.reset();
    }

    client->connected = false;
    client->id = -1;
    client->protocol = protocol_t::can;
    client->app_packetizing = false;
    client->host.clear();
    client->channel = 0;
    client->echo = false;
    client->filter_exclusive = false;
    client->inclusive.clear();
    client->exclusive_ids.clear();
    client->exclusive_masks.clear();
    client->last_error = RP1210_NO_ERRORS;
}

} // namespace rp1210
