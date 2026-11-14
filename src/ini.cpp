#include "ini.hpp"

#include <windows.h>

#include <cstdlib>
#include <vector>

namespace rp1210
{

namespace
{

constexpr const char *ini_name = "CANSUB32.ini";

/* RP1210 expects the vendor INI in the Windows directory. A copy beside the DLL is honoured first so
 * the driver can be tried without writing to a system directory. */
std::string locate_ini()
{
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&locate_ini), &self)
        && GetModuleFileNameA(self, path, sizeof(path)) > 0)
    {
        std::string beside(path);
        const size_t slash = beside.find_last_of('\\');

        if (slash != std::string::npos)
        {
            beside = beside.substr(0, slash + 1) + ini_name;

            if (GetFileAttributesA(beside.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                return beside;
            }
        }
    }

    char windows[MAX_PATH] = {};

    if (GetWindowsDirectoryA(windows, sizeof(windows)) > 0)
    {
        return std::string(windows) + "\\" + ini_name;
    }

    return ini_name;
}

std::string read_key(const std::string &path, const std::string &section, const std::string &key)
{
    char value[512] = {};

    GetPrivateProfileStringA(section.c_str(), key.c_str(), "", value, sizeof(value), path.c_str());

    return std::string(value);
}

/* DeviceParams carries the vendor-specific mapping. Two forms:
 *
 *   index=<n>,channel=<n>   the n-th device found by discovery (the default, nothing to configure)
 *   host=<name>,channel=<n> one particular device, by hostname or address
 *
 * Unknown keys are ignored, so a file written for a later version still loads here. */
void parse_device_params(const std::string &params, device_entry_t &entry)
{
    size_t pos = 0;

    while (pos < params.size())
    {
        const size_t comma = params.find(',', pos);
        const std::string item = params.substr(pos, (comma == std::string::npos) ? comma : comma - pos);
        const size_t equals = item.find('=');

        if (equals != std::string::npos)
        {
            std::string name = item.substr(0, equals);
            std::string value = item.substr(equals + 1);

            /* Trim, since INI values are often spaced for readability. */
            while (!name.empty() && name.front() == ' ')
            {
                name.erase(name.begin());
            }

            while (!value.empty() && value.back() == ' ')
            {
                value.pop_back();
            }

            if (name == "host")
            {
                entry.host = value;
            }
            else if (name == "channel")
            {
                entry.channel = atoi(value.c_str());
            }
            else if (name == "index")
            {
                /* Which discovered device, counting from one. Only used when no host is given. */
                entry.index = atoi(value.c_str());
            }
        }

        if (comma == std::string::npos)
        {
            break;
        }

        pos = comma + 1;
    }
}

} // namespace

std::string ini_path()
{
    static const std::string path = locate_ini();

    return path;
}

std::string vendor_key(const std::string &key)
{
    const std::string path = ini_path();

    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        return std::string();
    }

    return read_key(path, "VendorInformation", key);
}

bool find_device(int device_id, device_entry_t &entry)
{
    const std::string path = ini_path();

    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }

    const std::string section = "DeviceInformation" + std::to_string(device_id);

    /* An absent DeviceID key means the section is not there. */
    const std::string id = read_key(path, section, "DeviceID");

    if (id.empty() || atoi(id.c_str()) != device_id)
    {
        return false;
    }

    entry.device_id = device_id;
    entry.name = read_key(path, section, "DeviceName");
    entry.description = read_key(path, section, "DeviceDescription");

    parse_device_params(read_key(path, section, "DeviceParams"), entry);

    /* A channel is always required. A host is not: without one the driver discovers the device, so
     * demanding it here would reject exactly the entries that need no configuration. */
    return entry.channel >= 1;
}

} // namespace rp1210
