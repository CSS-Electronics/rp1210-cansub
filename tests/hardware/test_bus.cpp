/* Integration tests against a real CANsub with CH1 and CH2 wired together, so frames sent on one
 * channel are received on the other. Mirrors the parts of the python-can-cansub suite that make
 * sense here: open/close, send/recv, filters, error frames.
 *
 * Usage: test_bus <host>     e.g. test_bus <device-id>-eth.local */

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cansub/bus.hpp"

using namespace cansub;

namespace
{

const char *host = nullptr;
int failures = 0;

void check(bool condition, const char *what)
{
    printf("  %-52s %s\n", what, condition ? "ok" : "FAIL");

    failures += condition ? 0 : 1;
}

config_t default_config()
{
    config_t config;

    config.bitrate = 250000;

    return config;
}

/* Drains anything already queued so a test starts from a known state. */
void drain(bus &can)
{
    frame_t frame;

    while (ok(can.recv(frame, 50)))
    {
    }
}

void test_open_close()
{
    printf("open / close\n");

    bus can;

    check(ok(can.open(host, 1, default_config())), "open channel 1");
    check(can.is_open(), "reports open");
    check(can.api_version() == "05.00", "api version 05.00");
    check(!can.device_id().empty(), "device id read");
    check(can.timing().bitrate() == 250000, "timing resolves to 250 kbit/s");
    check(can.timing().sample_point_tenths() == 800, "sample point 80.0%");

    /* A second client on the same channel must be refused, since the device allows only one. */
    bus second;
    const status_t busy = second.open(host, 1, default_config());

    check(!ok(busy), "second client on the same channel refused");

    can.close();
    check(!can.is_open(), "reports closed");

    /* Reopening proves close really released the channel. */
    check(ok(can.open(host, 1, default_config())), "reopen after close");
    can.close();
}

/* The device may be reached by name or by IPv4 literal. Both verify the chain against the embedded
 * CANsub root; only the name check differs, matching python-can-cansub's assert_hostname=False path.
 * Nothing needs installing in the Windows certificate store either way. */
void test_tls_by_ip()
{
    printf("TLS by name and by IP\n");

    /* Resolve the device's own name so the test needs no hardcoded address; the USB link is
     * DHCP-addressed and changes between sessions. */
    std::string ip;

    {
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo *result = nullptr;

        if (getaddrinfo(host, "443", &hints, &result) == 0 && result != nullptr)
        {
            char text[INET_ADDRSTRLEN] = {};
            auto *addr = reinterpret_cast<sockaddr_in *>(result->ai_addr);

            inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text));
            ip = text;
            freeaddrinfo(result);
        }
    }

    if (ip.empty())
    {
        printf("  %-52s %s\n", "resolve device name", "skipped (name did not resolve)");

        return;
    }

    printf("  %s resolves to %s\n", host, ip.c_str());

    bus by_name;
    check(ok(by_name.open(host, 1, default_config())), "open by hostname");
    by_name.close();

    bus by_ip;
    const status_t result = by_ip.open(ip, 1, default_config());

    check(ok(result), "open by IP literal");

    if (ok(result))
    {
        check(by_ip.device_id() == "1a8788d1" || !by_ip.device_id().empty(),
              "device reachable by IP");
    }
    else
    {
        printf("    %s\n", by_ip.last_error().c_str());
    }

    by_ip.close();
}

void test_send_recv()
{
    printf("send / recv across CH1-CH2\n");

    bus tx;
    bus rx;

    if (!ok(tx.open(host, 1, default_config())) || !ok(rx.open(host, 2, default_config())))
    {
        printf("  FAIL could not open both channels: %s / %s\n",
            tx.last_error().c_str(), rx.last_error().c_str());
        failures++;

        return;
    }

    drain(rx);

    /* Standard identifier. */
    const uint8_t payload[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    frame_t sent = frame_t::make(0x123, payload, sizeof(payload));

    check(ok(tx.send(sent)), "send standard frame on CH1");

    frame_t got;
    const status_t received = rx.recv(got, 2000);

    check(ok(received), "received on CH2");

    if (ok(received))
    {
        check(got.id == 0x123, "identifier preserved");
        check(!got.extended, "standard identifier flag");
        check(got.len == sizeof(payload), "payload length preserved");
        check(memcmp(got.data, payload, sizeof(payload)) == 0, "payload preserved");
        check(!got.tx, "marked as received, not transmitted");
        check(got.us > 0, "device timestamp present");
    }

    /* Extended identifier. */
    const uint8_t two[] = {0xAA, 0xBB};
    frame_t ext = frame_t::make(0x1ABCDEF, two, sizeof(two), true);

    check(ok(tx.send(ext)), "send extended frame on CH1");

    if (ok(rx.recv(got, 2000)))
    {
        check(got.id == 0x1ABCDEF, "extended identifier preserved");
        check(got.extended, "extended identifier flag");
        check(got.len == 2, "extended payload length");
    }
    else
    {
        check(false, "extended frame received");
    }

    /* The reverse direction, so neither channel is assumed to be the sender. */
    drain(tx);
    frame_t back = frame_t::make(0x321, two, sizeof(two));

    check(ok(rx.send(back)), "send on CH2");

    if (ok(tx.recv(got, 2000)))
    {
        check(got.id == 0x321, "received on CH1");
    }
    else
    {
        check(false, "received on CH1");
    }

    tx.close();
    rx.close();
}

void test_filters()
{
    printf("hardware filters\n");

    bus tx;
    bus rx;

    config_t filtered = default_config();
    filtered.filters.push_back(filter_t::id(0x200));

    if (!ok(tx.open(host, 1, default_config())) || !ok(rx.open(host, 2, filtered)))
    {
        printf("  FAIL could not open: %s / %s\n", tx.last_error().c_str(), rx.last_error().c_str());
        failures++;

        return;
    }

    drain(rx);

    const uint8_t one[] = {0x01};

    /* Blocked identifier first, then the accepted one. If the filter leaks, the blocked frame
     * arrives first and the identifier check catches it. */
    check(ok(tx.send(frame_t::make(0x201, one, 1))), "send blocked identifier 0x201");
    check(ok(tx.send(frame_t::make(0x200, one, 1))), "send accepted identifier 0x200");

    frame_t got;

    if (ok(rx.recv(got, 2000)))
    {
        check(got.id == 0x200, "only the accepted identifier passed the filter");
    }
    else
    {
        check(false, "accepted identifier received");
    }

    check(!ok(rx.recv(got, 300)), "nothing further arrived");

    /* Widening at runtime must take effect. */
    check(ok(rx.set_filters({})), "set_filters to accept-all");
    drain(rx);
    check(ok(tx.send(frame_t::make(0x201, one, 1))), "resend 0x201");

    if (ok(rx.recv(got, 2000)))
    {
        check(got.id == 0x201, "previously blocked identifier now passes");
    }
    else
    {
        check(false, "previously blocked identifier now passes");
    }

    tx.close();
    rx.close();
}

void test_receive_own_messages()
{
    printf("transmit acknowledgements\n");

    config_t echo = default_config();
    echo.receive_own_messages = true;

    bus tx;
    bus rx;

    if (!ok(tx.open(host, 1, echo)) || !ok(rx.open(host, 2, default_config())))
    {
        printf("  FAIL could not open: %s / %s\n", tx.last_error().c_str(), rx.last_error().c_str());
        failures++;

        return;
    }

    drain(tx);

    const uint8_t one[] = {0x42};

    check(ok(tx.send(frame_t::make(0x111, one, 1))), "send with tx_ack_frames enabled");

    frame_t got;

    if (ok(tx.recv(got, 2000)))
    {
        check(got.id == 0x111, "own frame echoed back");
        check(got.tx, "echo is marked tx");
        check(got.us > 0, "echo carries a transmission timestamp");
    }
    else
    {
        check(false, "own frame echoed back");
    }

    tx.close();
    rx.close();
}

void test_error_frames()
{
    printf("error frames\n");

    /* With CH2 closed there is no second node to acknowledge, so transmitting on CH1 provokes ack
     * errors. That is exactly the condition a customer hits with an unterminated or unplugged bus,
     * and it is the only error the device can be provoked into reporting without special hardware. */
    config_t errors = default_config();
    errors.error_frames = true;
    errors.auto_reset = true;

    bus can;

    if (!ok(can.open(host, 1, errors)))
    {
        printf("  FAIL could not open: %s\n", can.last_error().c_str());
        failures++;

        return;
    }

    drain(can);

    const uint8_t one[] = {0x01};

    can.send(frame_t::make(0x7FF, one, 1));

    frame_t got;
    bool saw_error = false;

    for (int i = 0; i < 20 && !saw_error; i++)
    {
        if (ok(can.recv(got, 200)) && got.is_error)
        {
            saw_error = true;
            printf("  saw %s\n", error_type_string(got.error));
        }
    }

    /* Not a hard failure: whether an error frame appears depends on how the bus is wired right
     * now. Reported either way so the run is honest about what it proved. */
    printf("  %-52s %s\n", "error frame observed with no acknowledging node",
        saw_error ? "yes" : "no (CH2 may be open and acknowledging)");

    can.close();
}

} // namespace

void test_can_fd()
{
    printf("CAN FD across CH1-CH2\n");

    const uint32_t rates[][2] = {{500000, 2000000}, {1000000, 4000000}};

    for (const auto &rate : rates)
    {
        config_t config = default_config();

        config.bitrate = rate[0];
        config.data_bitrate = rate[1];

        bus tx;
        bus rx;

        if (!ok(tx.open(host, 1, config)) || !ok(rx.open(host, 2, config)))
        {
            printf("  FAIL could not open both channels at %u/%u: %s / %s\n", rate[0], rate[1],
                   tx.last_error().c_str(), rx.last_error().c_str());
            failures++;

            continue;
        }

        check(tx.data_timing().bitrate() == rate[1], "data timing resolves to the data rate");

        drain(rx);

        for (int brs = 0; brs <= 1; brs++)
        {
            frame_t sent;

            sent.id = 0x18FF1234;
            sent.extended = true;
            sent.fd = true;
            sent.brs = (brs != 0);
            sent.len = 64;

            for (int i = 0; i < 64; i++)
            {
                sent.data[i] = (uint8_t)(i * 3 + brs);
            }

            char what[96];

            snprintf(what, sizeof(what), "%u/%u: send 64-byte FD frame%s", rate[0], rate[1],
                     brs ? " with BRS" : "");
            check(ok(tx.send(sent)), what);

            frame_t got;

            snprintf(what, sizeof(what), "%u/%u: FD frame%s received intact", rate[0], rate[1],
                     brs ? " with BRS" : "");
            check(ok(rx.recv(got, 2000)) && got.fd && got.brs == sent.brs && got.len == 64
                      && got.id == sent.id && got.extended
                      && memcmp(got.data, sent.data, 64) == 0,
                  what);
        }
    }

    /* A classic channel has only a placeholder data phase, so FD frames are refused. */
    bus classic;

    if (ok(classic.open(host, 1, default_config())))
    {
        frame_t fd;

        fd.id = 0x123;
        fd.fd = true;
        fd.len = 12;

        check(classic.send(fd) == status_t::invalid_argument,
              "FD frame refused on a classic channel");
        classic.close();
    }
    else
    {
        check(false, "open classic channel");
    }

    /* A listen-only channel sends nothing. */
    config_t listen = default_config();

    listen.listen_only = true;

    bus quiet;

    if (ok(quiet.open(host, 1, listen)))
    {
        const uint8_t one[] = {1};

        check(quiet.send(frame_t::make(0x123, one, 1)) == status_t::invalid_argument,
              "send refused on a listen-only channel");
    }
    else
    {
        printf("  (%s)\n", quiet.last_error().c_str());
        check(false, "open listen-only channel");
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("usage: test_bus <host>, e.g. test_bus <device-id>-eth.local\n");
        return 2;
    }

    host = argv[1];

    printf("device: %s\n\n", host);

    test_open_close();
    test_tls_by_ip();
    test_send_recv();
    test_filters();
    test_receive_own_messages();
    test_error_frames();
    test_can_fd();

    printf("\n%s\n", failures == 0 ? "all bus tests passed" : "bus tests FAILED");

    return (failures == 0) ? 0 : 1;
}
