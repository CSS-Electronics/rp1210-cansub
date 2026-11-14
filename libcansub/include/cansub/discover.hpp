#ifndef CANSUB_DISCOVER_HPP
#define CANSUB_DISCOVER_HPP

/* Finds CANsub devices on the local network, so a caller does not have to be told a hostname.
 *
 * The device advertises itself over mDNS and DNS-SD as a `_cansub._tcp` service on port 443, with
 * TXT records giving the REST API version and the number of CAN channels. That is enough to pick a
 * device, check it is one this library can talk to, and check the channel exists -- all before
 * opening anything.
 *
 * The reply also carries the device's address, so discovery does not depend on the operating
 * system resolving a `.local` name. That matters: on a machine with several network adapters, VPN
 * or otherwise, `.local` resolution is the part most likely to fail. */

#include <cstdint>
#include <string>
#include <vector>

#include "status.hpp"

namespace cansub
{

struct device_info_t
{
    std::string id;       /* Eight hex digits, as printed on the label, e.g. "97622d1f". */
    std::string hostname; /* "97622d1f-usb.local" */
    std::string address;  /* "10.216.248.1", from the advertisement rather than a name lookup. */
    uint16_t port = 443;

    std::string api;  /* REST API version, e.g. "05.00". Empty if the device did not say. */
    int channels = 0; /* CAN channels the device has. Zero if the device did not say. */

    bool usb = false; /* Reached over the USB network interface rather than Ethernet. */

    /* Whether this library speaks the device's API version. A device that fails this is still
     * reported, so a caller can say which firmware is in the way instead of staying silent. */
    bool supported() const;
};

/* Browses for devices and returns them sorted by id, so the order is stable for a given set.
 *
 * Waits up to timeout_ms for replies; a device usually answers in a few milliseconds. Returns ok
 * with an empty list when nothing answered, which is not an error: there may be no device attached.
 */
status_t discover(std::vector<device_info_t> &out, int timeout_ms = 1000);

} // namespace cansub

#endif
