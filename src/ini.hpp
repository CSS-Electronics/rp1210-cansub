#ifndef CANSUB_RP1210_INI_HPP
#define CANSUB_RP1210_INI_HPP

/* Reads the vendor INI that tells the driver which device id maps to which CANsub channel.
 *
 * RP1210 puts the INI next to the DLL in the Windows directory and identifies devices by a numeric
 * DeviceID. The mapping from that number to a hostname and CAN channel is vendor-defined, so it
 * lives in DeviceParams here. */

#include <string>

namespace rp1210
{

struct device_entry_t
{
    int device_id = 0;

    /* Where the device is. An explicit host= in DeviceParams pins one particular unit, by name or
     * by address, which is what a fixed installation wants and what an environment with mDNS
     * blocked needs. Left empty, the driver discovers whatever is attached and takes the index-th
     * device, so one INI works on every machine and nobody edits a device id into a file. */
    std::string host;
    int index = 0;

    int channel = 0;
    std::string name;
    std::string description;

    bool discover() const { return host.empty(); }
};

/* Looks up a device by its RP1210 DeviceID. Returns false when the INI or the entry is missing. */
bool find_device(int device_id, device_entry_t &entry);

/* The INI path actually used, for diagnostics. */
std::string ini_path();

/* Reads a key out of [VendorInformation], for the DebugFile and DebugLevel settings RP1210 defines
 * there. Returns an empty string when the INI or the key is missing. */
std::string vendor_key(const std::string &key);

} // namespace rp1210

#endif
