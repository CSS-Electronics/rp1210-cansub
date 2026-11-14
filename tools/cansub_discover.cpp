/* Lists the CANsub devices on the local network.
 *
 * Answers "what is attached, and can this driver talk to it" without anyone reading a label or
 * editing a file. Ships alongside the RP1210 driver because those are the two questions a customer
 * asks first, and a bare error code answers neither.
 *
 * Usage: cansub_discover [timeout-ms] */

#include <cstdio>
#include <cstdlib>

#include "cansub/discover.hpp"

int main(int argc, char **argv)
{
    const int timeout = (argc > 1) ? atoi(argv[1]) : 1500;

    printf("Looking for CANsub devices (%d ms) ...\n\n", timeout);

    std::vector<cansub::device_info_t> devices;
    const cansub::status_t result = cansub::discover(devices, timeout);

    if (!cansub::ok(result))
    {
        printf("Discovery failed: %s\n", cansub::status_string(result));

        return 1;
    }

    if (devices.empty())
    {
        printf("No devices answered.\n\n");
        printf("The device announces itself over mDNS, so check that:\n");
        printf("  - it is connected and powered, and its display is on\n");
        printf("  - for USB, the CANsub network adapter exists (run \"ipconfig\" and look for an\n");
        printf("    adapter with an address like 10.x.x.100)\n");
        printf("  - a firewall is not blocking UDP port 5353 on that adapter\n");
        printf("\nA device can still be reached by name or address without discovery.\n");

        return 1;
    }

    printf("  %-10s %-24s %-16s %-6s %-8s %s\n", "DEVICE ID", "HOSTNAME", "ADDRESS", "API",
           "CHANNELS", "");

    for (const cansub::device_info_t &device : devices)
    {
        printf("  %-10s %-24s %-16s %-6s %-8d %s\n", device.id.c_str(), device.hostname.c_str(),
               device.address.empty() ? "-" : device.address.c_str(),
               device.api.empty() ? "-" : device.api.c_str(), device.channels,
               device.supported() ? "" : "<- API NOT SUPPORTED");
    }

    printf("\n%zu device(s) found.\n", devices.size());

    bool unsupported = false;

    for (const cansub::device_info_t &device : devices)
    {
        if (!device.supported())
        {
            unsupported = true;
        }
    }

    if (unsupported)
    {
        printf("\nOne or more devices run a REST API version this build does not speak. That is a\n");
        printf("firmware difference: API 05.00 needs CANsub firmware 02.05.00 or later. See\n");
        printf("https://canlogger.csselectronics.com/cansub-docs/ for the update.\n");
    }

    return 0;
}
