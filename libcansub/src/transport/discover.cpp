/* DNS-SD discovery of CANsub devices. See discover.hpp for what it is for.
 *
 * The query goes out on every IPv4 interface rather than just the default route: a USB-attached
 * CANsub sits on its own little subnet that is never the default route, so a single-interface
 * browse would miss exactly the case that matters most. */

#include "cansub/discover.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
/* iphlpapi.h must follow winsock2.h. */
#include <iphlpapi.h>

#include <algorithm>
#include <map>

#include "mdns.hpp"
#include "sdk/convert.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace cansub
{

namespace
{

constexpr const char *service = "_cansub._tcp.local";
constexpr const char *multicast_group = "224.0.0.251";
constexpr uint16_t multicast_port = 5353;

/* Winsock has to be started per process and stopped as many times as it was started. Doing it once
 * on first use and never unwinding is deliberate: a DLL cannot know when its host is finished, and
 * calling WSACleanup while the host still has sockets open would break the host. */
void ensure_winsock()
{
    static bool started = false;

    if (!started)
    {
        WSADATA data;

        WSAStartup(MAKEWORD(2, 2), &data);
        started = true;
    }
}

std::vector<uint32_t> local_ipv4()
{
    std::vector<uint32_t> out;

    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);

    ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST
                                                     | GAA_FLAG_SKIP_DNS_SERVER,
                                        nullptr, (IP_ADAPTER_ADDRESSES *)buffer.data(), &size);

    if (result == ERROR_BUFFER_OVERFLOW)
    {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST
                                                   | GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, (IP_ADAPTER_ADDRESSES *)buffer.data(), &size);
    }

    if (result != NO_ERROR)
    {
        return out;
    }

    for (const IP_ADAPTER_ADDRESSES *adapter = (const IP_ADAPTER_ADDRESSES *)buffer.data();
         adapter != nullptr; adapter = adapter->Next)
    {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
        {
            continue;
        }

        for (const IP_ADAPTER_UNICAST_ADDRESS *unicast = adapter->FirstUnicastAddress;
             unicast != nullptr; unicast = unicast->Next)
        {
            if (unicast->Address.lpSockaddr->sa_family != AF_INET)
            {
                continue;
            }

            const sockaddr_in *in = (const sockaddr_in *)unicast->Address.lpSockaddr;

            out.push_back(in->sin_addr.s_addr);
        }
    }

    return out;
}

/* The instance name is "<device-id>-usb" or "<device-id>-eth". */
void split_instance(const std::string &instance, std::string &id, bool &usb)
{
    const size_t dash = instance.rfind('-');

    if (dash == std::string::npos)
    {
        id = instance;
        usb = false;

        return;
    }

    id = instance.substr(0, dash);
    usb = instance.compare(dash + 1, std::string::npos, "usb") == 0;
}

std::string txt_value(const std::vector<std::string> &txt, const std::string &key)
{
    for (const std::string &item : txt)
    {
        if (item.size() > key.size() + 1 && item.compare(0, key.size(), key) == 0
            && item[key.size()] == '=')
        {
            return item.substr(key.size() + 1);
        }
    }

    return std::string();
}

/* Strips the trailing ".local" so a name reads the way a user would type it. */
std::string trim_local(const std::string &name)
{
    const std::string suffix = ".local";

    if (name.size() > suffix.size()
        && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
    {
        return name;
    }

    return name;
}

} // namespace

bool device_info_t::supported() const
{
    /* An advertisement without an api record is not evidence of incompatibility, so it is given the
     * benefit of the doubt and left for bus::open to judge. Otherwise the SDK decides. */
    return api.empty() || sdk::api_supported(api);
}

status_t discover(std::vector<device_info_t> &out, int timeout_ms)
{
    out.clear();
    ensure_winsock();

    const std::vector<uint32_t> interfaces = local_ipv4();

    if (interfaces.empty())
    {
        return status_t::ok; /* no network at all is not a failure to discover */
    }

    const std::vector<uint8_t> question = mdns::query(service, mdns::type_ptr);

    std::vector<SOCKET> sockets;

    for (uint32_t address : interfaces)
    {
        SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (socket_handle == INVALID_SOCKET)
        {
            continue;
        }

        BOOL reuse = TRUE;

        setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

        sockaddr_in local = {};

        local.sin_family = AF_INET;
        local.sin_addr.s_addr = address;
        local.sin_port = 0;

        if (bind(socket_handle, (const sockaddr *)&local, sizeof(local)) != 0)
        {
            closesocket(socket_handle);

            continue;
        }

        /* Send the multicast out of this interface specifically. */
        in_addr interface_address = {};

        interface_address.s_addr = address;
        setsockopt(socket_handle, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&interface_address,
                   sizeof(interface_address));

        sockaddr_in group = {};

        group.sin_family = AF_INET;
        group.sin_port = htons(multicast_port);
        inet_pton(AF_INET, multicast_group, &group.sin_addr);

        sendto(socket_handle, (const char *)question.data(), (int)question.size(), 0,
               (const sockaddr *)&group, sizeof(group));

        sockets.push_back(socket_handle);
    }

    if (sockets.empty())
    {
        return status_t::connect_failed;
    }

    /* Collect by service instance: the answer arrives spread across PTR, SRV, TXT and A records,
     * and not necessarily in one packet. */
    struct partial_t
    {
        std::string hostname;
        std::string address;
        uint16_t port = 443;
        std::string api;
        int channels = 0;
        bool seen = false;
    };

    std::map<std::string, partial_t> devices;
    std::map<std::string, std::string> addresses; /* hostname -> dotted quad */

    const DWORD deadline = GetTickCount() + (DWORD)(timeout_ms > 0 ? timeout_ms : 0);

    for (;;)
    {
        const DWORD now = GetTickCount();

        if (now >= deadline)
        {
            break;
        }

        fd_set readable;

        FD_ZERO(&readable);

        SOCKET highest = 0;

        for (SOCKET handle : sockets)
        {
            FD_SET(handle, &readable);
            highest = (handle > highest) ? handle : highest;
        }

        const DWORD left = deadline - now;

        timeval wait = {};

        wait.tv_sec = (long)(left / 1000);
        wait.tv_usec = (long)((left % 1000) * 1000);

        const int ready = select((int)highest + 1, &readable, nullptr, nullptr, &wait);

        if (ready <= 0)
        {
            break;
        }

        for (SOCKET handle : sockets)
        {
            if (!FD_ISSET(handle, &readable))
            {
                continue;
            }

            uint8_t buffer[9000];
            sockaddr_in from = {};
            int from_size = sizeof(from);

            const int received = recvfrom(handle, (char *)buffer, sizeof(buffer), 0,
                                          (sockaddr *)&from, &from_size);

            if (received <= 0)
            {
                continue;
            }

            std::vector<mdns::record_t> records;

            mdns::parse(buffer, (size_t)received, records);

            for (const mdns::record_t &record : records)
            {
                if (record.type == mdns::type_ptr && record.name == service)
                {
                    devices[record.target].seen = true;
                }
                else if (record.type == mdns::type_srv)
                {
                    partial_t &device = devices[record.name];

                    device.hostname = trim_local(record.target);
                    device.port = record.port;
                }
                else if (record.type == mdns::type_txt)
                {
                    partial_t &device = devices[record.name];
                    const std::string channels = txt_value(record.txt, "channels");

                    device.api = txt_value(record.txt, "api");
                    device.channels = channels.empty() ? 0 : atoi(channels.c_str());
                }
                else if (record.type == mdns::type_a)
                {
                    addresses[record.name] = record.target;
                }
            }
        }
    }

    for (SOCKET handle : sockets)
    {
        closesocket(handle);
    }

    for (const auto &entry : devices)
    {
        const std::string &instance = entry.first;
        const partial_t &partial = entry.second;

        /* A PTR on its own says a device exists but not where; without a host there is nothing a
         * caller could connect to, so it is not reported. */
        if (partial.hostname.empty())
        {
            continue;
        }

        device_info_t info;

        /* The instance name still carries the service suffix. */
        std::string bare = instance;
        const std::string suffix = std::string(".") + service;

        if (bare.size() > suffix.size()
            && bare.compare(bare.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            bare.resize(bare.size() - suffix.size());
        }

        split_instance(bare, info.id, info.usb);

        info.hostname = partial.hostname;
        info.port = partial.port;
        info.api = partial.api;
        info.channels = partial.channels;

        const auto address = addresses.find(partial.hostname);

        if (address != addresses.end())
        {
            info.address = address->second;
        }

        out.push_back(info);
    }

    /* Sorted by id, and USB before Ethernet when one device advertises both, so a given set of
     * devices always maps to the same DeviceIDs. */
    std::sort(out.begin(), out.end(), [](const device_info_t &a, const device_info_t &b) {
        if (a.id != b.id)
        {
            return a.id < b.id;
        }

        return a.usb && !b.usb;
    });

    return status_t::ok;
}

} // namespace cansub
