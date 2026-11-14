/* Opens a CANsub channel and prints every frame until interrupted.
 *
 * Usage: dump [host] [channel] [bitrate] */

#include <cstdio>
#include <cstdlib>

#include "cansub/bus.hpp"

using namespace cansub;

int main(int argc, char **argv)
{
    const char *host = (argc > 1) ? argv[1] : "1a8788d1-usb.local";
    const int channel = (argc > 2) ? atoi(argv[2]) : 1;

    config_t config;
    config.bitrate = (argc > 3) ? (uint32_t)strtoul(argv[3], nullptr, 10) : 250000;
    config.error_frames = true;

    bus can;
    const status_t result = can.open(host, channel, config);

    if (!ok(result))
    {
        printf("open: %s\n  %s\n", status_string(result), can.last_error().c_str());

        return 1;
    }

    printf("%s ch%d @ %u bit/s  (device %s, fw %s, api %s)\n",
        host, channel, config.bitrate, can.device_id().c_str(),
        can.firmware_version().c_str(), can.api_version().c_str());
    printf("%-18s %-10s %-4s %s\n", "timestamp (us)", "id", "len", "data");

    while (true)
    {
        frame_t frame;
        const status_t got = can.recv(frame, 1000);

        if (got == status_t::timeout)
        {
            continue;
        }

        if (!ok(got))
        {
            printf("recv: %s\n  %s\n", status_string(got), can.last_error().c_str());
            break;
        }

        if (frame.is_error)
        {
            printf("%-18llu %s\n", (unsigned long long)frame.us, error_type_string(frame.error));
            continue;
        }

        printf("%-18llu %-10s %-4u ",
            (unsigned long long)frame.us,
            (frame.extended ? "ext" : "std"),
            frame.len);

        printf("0x%0*X  ", frame.extended ? 8 : 3, frame.id);

        for (uint8_t i = 0; i < frame.len; i++)
        {
            printf("%02X ", frame.data[i]);
        }

        printf("%s%s\n", frame.tx ? " [tx]" : "", frame.fd ? " [fd]" : "");
    }

    can.close();

    return 0;
}
