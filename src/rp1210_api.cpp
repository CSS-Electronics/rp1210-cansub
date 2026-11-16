/* The RP1210E entry points, translating to libcansub.
 *
 * Two protocols: generic CAN, and J1939 with the transport protocol and address claiming the
 * standard makes the adapter responsible for. ISO15765 and J1708 are refused rather than half
 * implemented; see README.md.
 *
 * The two protocols share these entry points but almost nothing else, so each one branches early
 * and the generic CAN path below is left exactly as it was when it was verified on hardware. */

#include "rp1210.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "cansub/discover.hpp"

#include "client.hpp"
#include "connect_params.hpp"
#include "ini.hpp"
#include "j1939.hpp"
#include "trace.hpp"

using namespace cansub;
using namespace rp1210;

namespace
{

/* Version reported by RP1210_ReadVersion and in the detailed strings.
 *
 * Keep these consistent with the RP1210 key in CANSUB32.ini, which declares B. Some applications
 * recognise only A and B. The driver was written from the E specification, but
 * for the generic CAN protocol the message formats are identical, so declaring B is accurate for
 * anything an application can rely on.
 *
 * The numeric revision below is a best reading, since no source available here pins the mapping from
 * revision letter to the ReadVersion numbers. Applications generally take the name from the INI
 * instead, so the risk is low. Listed as an open question in README.md. */
/* The driver's own version comes from project(VERSION) in CMakeLists.txt, the single source for the
 * release tag, the INI's Version key (checked at configure time) and these strings. RP1210_ReadVersion
 * has room for major and minor only. */
#ifndef RP1210_CANSUB_VERSION
#error "RP1210_CANSUB_VERSION is not defined: build with CMakeLists.txt"
#endif

constexpr const char *dll_major = RP1210_CANSUB_VERSION_MAJOR;
constexpr const char *dll_minor = RP1210_CANSUB_VERSION_MINOR;
constexpr const char *dll_version_info = "CANSUB32 " RP1210_CANSUB_VERSION; /* fits the 17-byte field */
constexpr const char *api_major = "2"; /* RP1210B */
constexpr const char *api_minor = "0";
constexpr const char *api_revision = "RP1210B";

/* Why the most recent RP1210_ClientConnect failed, in the library's own words.
 *
 * A failed connect has no client, so there is nothing for RP1210_GetLastErrorMsg to look up and the
 * explanation would otherwise be thrown away -- leaving an application holding a bare mnemonic like
 * ERR_DEVICE_NOT_SUPPORTED with nothing to act on. Keeping it here costs a string and turns
 * "the device is not supported" into "device API version 04.00 not supported (need 05.00)". */
std::mutex &connect_error_mutex()
{
    static std::mutex mutex;

    return mutex;
}

std::string &connect_error()
{
    static std::string text;

    return text;
}

void set_connect_error(const std::string &text)
{
    std::lock_guard<std::mutex> lock(connect_error_mutex());

    connect_error() = text;
}

/* Copies text into a caller-supplied buffer of unknown size. RP1210 leaves buffer sizes implicit for
 * the version calls, so the documented minimums are all that may be assumed. */
void copy_fixed(char *dest, const char *text, size_t size)
{
    if (dest == nullptr)
    {
        return;
    }

    strncpy_s(dest, size, text, size - 1);
}

/* The connect string is "<protocol>[:key=value[,key=value...]]". RP1210E section 14.15 also allows
 * a semicolon between groups, and applications append vendor tokens such as Channel= or
 * Transport=, so unknown keys are ignored rather than refused.
 *
 * Returns the protocol name in name[], which the caller compares whole. The earlier version tested
 * only the first three characters, so "CANopen" connected as generic CAN. */
void split_protocol(const char *protocol, char *name, size_t name_size)
{
    size_t i = 0;

    while (protocol[i] != '\0' && protocol[i] != ':' && protocol[i] != ',' && protocol[i] != ';'
           && i + 1 < name_size)
    {
        name[i] = protocol[i];
        i++;
    }

    name[i] = '\0';
}

bool protocol_is(const char *name, const char *want)
{
    return _stricmp(name, want) == 0;
}

/* "Baud=Auto" on J1939, done honestly: listen at each candidate rate and take the first that
 * produces traffic.
 *
 * Listen-only means the probe never acknowledges, so guessing wrong cannot disturb the bus -- which
 * is the only reason this is safe to do at all. The cost is up to about 1.4 s of connect time, and
 * a bus with nothing on it cannot be detected; an application that knows the rate should say so.
 * 250k first, then 500k: J1939-11 and J1939-14. */
bool probe_bitrate(const std::string &host, int channel, uint32_t &bitrate)
{
    static const uint32_t candidates[] = {250000, 500000};

    for (uint32_t rate : candidates)
    {
        bus probe;
        config_t config;

        config.bitrate = rate;
        config.listen_only = true;
        config.error_frames = false;

        if (!ok(probe.open(host, channel, config)))
        {
            continue;
        }

        /* Throw away whatever is already waiting before counting anything.
         *
         * Opening the channel can deliver frames that have nothing to do with the current bus
         * state, and counting those makes the probe claim a rate on evidence it did not gather:
         * measured on a silent loopback bus, eleven probes in a row "found traffic" at 250k and
         * every one after that correctly found none. On a 500k bus that would have settled at 250k.
         * So: flush, discard for a moment, and only then start counting. */
        probe.flush();

        const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);

        for (;;)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  settle - std::chrono::steady_clock::now())
                                  .count();

            if (left <= 0)
            {
                break;
            }

            frame_t discarded;

            if (probe.recv(discarded, (int)left) != status_t::ok)
            {
                break;
            }
        }

        probe.flush();

        int seen = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);

        /* Two frames rather than one: a single frame can be a fluke of a wrong-rate bus producing
         * something that happens to decode. */
        while (seen < 2)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();

            if (left <= 0)
            {
                break;
            }

            frame_t frame;

            if (probe.recv(frame, (int)left) != status_t::ok)
            {
                break;
            }

            if (!frame.is_error)
            {
                seen++;
            }
        }

        probe.close();

        if (seen >= 2)
        {
            bitrate = rate;

            return true;
        }
    }

    return false;
}

/* Turns a DeviceParams entry without a host into an actual device, by asking the network.
 *
 * This is what lets one INI work on every machine: real RP1210 adapters are enumerated by their
 * driver and their INIs name a kind of adapter rather than a serial number, and the CANsub can be
 * treated the same way because it advertises itself over DNS-SD.
 *
 * The advertisement carries the API version and the channel count, so the two mistakes that are
 * otherwise a bare error code get named here instead: firmware too old, and a channel the device
 * does not have. */
short resolve_device(device_entry_t &device)
{
    if (!device.discover())
    {
        return RP1210_NO_ERRORS;
    }

    std::vector<device_info_t> found;

    if (!ok(discover(found, 1200)))
    {
        set_connect_error("device discovery failed");

        return ERR_HARDWARE_NOT_RESPONDING;
    }

    if (found.empty())
    {
        set_connect_error("no CANsub answered on any network interface");

        return ERR_HARDWARE_NOT_RESPONDING;
    }

    const int index = (device.index > 0) ? device.index : 1;

    if ((size_t)index > found.size())
    {
        char text[160];

        snprintf(text, sizeof(text),
                 "DeviceID %d wants CANsub number %d but only %zu answered", device.device_id,
                 index, found.size());
        set_connect_error(text);

        return ERR_INVALID_DEVICE;
    }

    const device_info_t &info = found[(size_t)index - 1];

    if (!info.supported())
    {
        char text[160];

        snprintf(text, sizeof(text),
                 "CANsub %s runs API %s, need 05.00 (firmware 02.05.00 or later)", info.id.c_str(),
                 info.api.c_str());
        set_connect_error(text);

        return ERR_DEVICE_NOT_SUPPORTED;
    }

    if (info.channels > 0 && device.channel > info.channels)
    {
        char text[160];

        snprintf(text, sizeof(text), "CANsub %s has %d CAN channel(s), so channel %d does not exist",
                 info.id.c_str(), info.channels, device.channel);
        set_connect_error(text);

        return ERR_INVALID_DEVICE;
    }

    /* Prefer the hostname, so the device certificate's name is checked and reaching the wrong
     * device is detectable. The advertised address is the fallback for a machine where `.local`
     * resolution does not work, which is common enough with several adapters or a VPN in play. */
    device.host = !info.hostname.empty() ? info.hostname : info.address;

    return RP1210_NO_ERRORS;
}

/* Reopens the channel, which several commands need because bit rate and transmit acknowledgement
 * are channel settings. For a J1939 client the worker has to be stopped first: it owns the bus, and
 * closing underneath a live recv would pull the object out from under it. */
short reopen_channel(client_t *client)
{
    const bool j1939_client = (client->protocol == protocol_t::j1939);

    if (j1939_client)
    {
        client->stop_worker();
    }

    client->can->close();

    if (!ok(client->can->open(client->host, client->channel, client->config)))
    {
        return ERR_HARDWARE_NOT_RESPONDING;
    }

    if (j1939_client)
    {
        if (!ok(client->can->set_hw_filters({filter_t::accept_all(true)})))
        {
            return ERR_HARDWARE_NOT_RESPONDING;
        }

        /* Transfers cannot survive the channel going down, and there is nobody left to abort to.
         * The address claim does survive, but peers may have re-arbitrated while we were away, so
         * it is restated. */
        client->engine->flush();
        client->engine->reannounce();
        client->start_worker();
    }

    return RP1210_NO_ERRORS;
}

/* Maps an engine outcome onto the RP1210 error code an application expects. */
short error_from(j1939::result_t result)
{
    switch (result)
    {
        case j1939::result_t::ok:                    return RP1210_NO_ERRORS;
        case j1939::result_t::too_long:              return ERR_MESSAGE_TOO_LONG;
        case j1939::result_t::address_never_claimed: return ERR_ADDRESS_NEVER_CLAIMED;
        case j1939::result_t::address_lost:          return ERR_ADDRESS_LOST;
        case j1939::result_t::queue_full:            return ERR_TX_QUEUE_FULL;
        case j1939::result_t::claim_failed:          return ERR_ADDRESS_CLAIM_FAILED;
        default:                                     return ERR_MESSAGE_NOT_SENT;
    }
}

/* RP1210_Set_J1939_Baud carries the same baud identifiers as the CAN command, but applications
 * disagree about whether to send them as ASCII digits or as raw numbers: the reference library
 * emits 0x04..0x07 while the existing CAN test sends '4'..'7'. Accept both here rather than widen
 * bitrate_from_baud_id, whose behaviour a passing test already pins. */
uint32_t bitrate_from_j1939_baud_id(unsigned char id)
{
    switch (id)
    {
        case '4': case 0x04: return 125000;
        case '5': case 0x05: return 250000;
        case '6': case 0x06: return 500000;
        case '7': case 0x07: return 1000000;
        default:             return 0;
    }
}

uint32_t bitrate_from_baud_id(char id)
{
    switch (id)
    {
        case '4': return 125000;
        case '5': return 250000;
        case '6': return 500000;
        case '7': return 1000000;
        default:  return 0;
    }
}

/* Applies the software-side exclusive filter. The CANsub's hardware filters are accept-only, so a
 * block list has to be enforced here (RP1210B added exclusive filtering). */
bool blocked(const client_t &client, const frame_t &frame)
{
    if (!client.filter_exclusive)
    {
        return false;
    }

    for (size_t i = 0; i < client.exclusive_ids.size(); i++)
    {
        const uint32_t mask = (i < client.exclusive_masks.size()) ? client.exclusive_masks[i]
                                                                  : 0xFFFFFFFFu;

        if ((frame.id & mask) == (client.exclusive_ids[i] & mask))
        {
            return true;
        }
    }

    return false;
}

/* Returned by j1939_command when the command is not one it owns, so the shared switch runs instead.
 * No RP1210 code is negative, so this cannot collide with a real answer. */
constexpr short command_not_handled = -1;

/* The commands that mean something different, or something at all, for a J1939 client. */
short j1939_command(client_t *client, short number, const char *data, short size)
{
    switch (number)
    {
        case RPCMD_RESET_DEVICE:
        {
            return reopen_channel(client);
        }

        case RPCMD_SET_ALL_FILTERS_STATES_TO_PASS:
        {
            std::lock_guard<std::mutex> lock(client->engine_mutex);

            client->engine->set_gate(j1939::gate_t::all);

            return RP1210_NO_ERRORS;
        }

        case RPCMD_SET_ALL_FILTERS_STATES_TO_DISCARD:
        {
            /* Only delivery stops. The hardware stays wide open, because the driver still has a
             * transport protocol to run and an address to defend. */
            std::lock_guard<std::mutex> lock(client->engine_mutex);

            client->engine->set_gate(j1939::gate_t::none);

            return RP1210_NO_ERRORS;
        }

        case RPCMD_SET_MESSAGE_FILTERING_FOR_J1939:
        {
            /* Flags | PGN (3, little endian) | 0 | source | destination. The zero byte was the
             * priority filter before it was withdrawn from the standard. */
            if (data == nullptr || size < 7)
            {
                return ERR_INVALID_COMMAND; /* see note on E-only codes */
            }

            j1939::filter_t filter;

            filter.flags = (uint8_t)data[0];
            filter.pgn = (uint32_t)(unsigned char)data[1] | ((uint32_t)(unsigned char)data[2] << 8)
                         | ((uint32_t)(unsigned char)data[3] << 16);
            filter.sa = (uint8_t)data[5];
            filter.da = (uint8_t)data[6];

            if ((filter.flags & ~j1939::filter_flags_mask) != 0)
            {
                return ERR_INVALID_COMMAND; /* see note on E-only codes */
            }

            std::lock_guard<std::mutex> lock(client->engine_mutex);

            return client->engine->add_filter(filter) ? RP1210_NO_ERRORS : ERR_MAX_FILTERS_EXCEEDED;
        }

        case RPCMD_SET_J1939_FILTER_TYPE:
        {
            if (data == nullptr || size < 1)
            {
                return ERR_INVALID_COMMAND; /* see note on E-only codes */
            }

            if (data[0] != RP1210_FILTER_INCLUSIVE && data[0] != RP1210_FILTER_EXCLUSIVE)
            {
                return ERR_INVALID_COMMAND; /* see note on E-only codes */
            }

            std::lock_guard<std::mutex> lock(client->engine_mutex);

            client->engine->set_filter_exclusive(data[0] == RP1210_FILTER_EXCLUSIVE);

            return RP1210_NO_ERRORS;
        }

        case RPCMD_SET_J1939_INTERPACKET_TIME:
        {
            /* Four bytes, little endian, milliseconds. */
            if (data == nullptr || size < 4)
            {
                return ERR_INVALID_COMMAND; /* see note on E-only codes */
            }

            const uint32_t ms = (uint32_t)(unsigned char)data[0]
                                | ((uint32_t)(unsigned char)data[1] << 8)
                                | ((uint32_t)(unsigned char)data[2] << 16)
                                | ((uint32_t)(unsigned char)data[3] << 24);

            std::lock_guard<std::mutex> lock(client->engine_mutex);

            client->engine->set_interpacket_ms(ms);

            return RP1210_NO_ERRORS;
        }

        case RPCMD_PROTECT_J1939_ADDRESS:
        {
            /* Address | NAME (8) | blocking status. */
            if (data == nullptr || size < 10)
            {
                return ERR_INVALID_COMMAND; /* see note on E-only codes */
            }

            const unsigned char address = (unsigned char)data[0];
            const unsigned char blocking = (unsigned char)data[9];

            if (!j1939::address_is_claimable(address))
            {
                return ERR_CANNOT_CLAIM_BROADCAST_ADDRESS;
            }

            uint8_t name[8];

            memcpy(name, data + 1, sizeof(name));

            {
                std::lock_guard<std::mutex> lock(client->engine_mutex);

                if (client->engine->begin_claim(address, name) != j1939::result_t::ok)
                {
                    /* One address per client, which is what J1939Addresses=1 declares. */
                    return ERR_OUT_OF_ADDRESS_RESOURCES;
                }

                client->engine->tick();
            }

            /* 2 asks to return before the contention window closes; anything else waits for it. */
            if (blocking == 2)
            {
                return RP1210_NO_ERRORS;
            }

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);

            for (;;)
            {
                {
                    std::lock_guard<std::mutex> lock(client->engine_mutex);
                    const j1939::claim_state_t state = client->engine->claim_state();

                    if (state == j1939::claim_state_t::lost)
                    {
                        return ERR_ADDRESS_CLAIM_FAILED;
                    }

                    if (state == j1939::claim_state_t::claimed)
                    {
                        return RP1210_NO_ERRORS;
                    }
                }

                if (std::chrono::steady_clock::now() >= deadline)
                {
                    return ERR_ADDRESS_CLAIM_FAILED;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        case RPCMD_RELEASE_J1939_ADDRESS:
        {
            if (data == nullptr || size < 1)
            {
                return ERR_INVALID_COMMAND; /* see note on E-only codes */
            }

            std::lock_guard<std::mutex> lock(client->engine_mutex);

            return (client->engine->release_claim((uint8_t)data[0]) == j1939::result_t::ok)
                       ? RP1210_NO_ERRORS
                       : ERR_ADDRESS_NEVER_CLAIMED;
        }

        case RPCMD_SET_J1939_BAUD:
        {
            if (data == nullptr || size < 2)
            {
                return ERR_INVALID_COMMAND; /* see note on E-only codes */
            }

            const uint32_t bitrate = bitrate_from_j1939_baud_id((unsigned char)data[1]);

            if (bitrate == 0)
            {
                return ERR_J1939_BAUD_SET_NONSTANDARD;
            }

            client->config.bitrate = bitrate;

            return (reopen_channel(client) == RP1210_NO_ERRORS) ? RP1210_NO_ERRORS
                                                                : ERR_CANNOT_SET_CAN_BAUDRATE;
        }

        case RPCMD_SET_BROADCAST_FOR_J1939:
        {
            /* Periodic broadcast lists are not implemented. A clean refusal is easier to diagnose
             * than a list an application can add to and then never see transmitted. */
            return ERR_COMMAND_NOT_SUPPORTED;
        }

        case RPCMD_ECHO_TRANSMITTED_MESSAGES:
        {
            if (data == nullptr || size < 1)
            {
                return ERR_INVALID_COMMAND; /* see note on E-only codes */
            }

            const bool on = (data[0] == RP1210_ECHO_ON);

            if (on == client->echo)
            {
                return RP1210_NO_ERRORS;
            }

            /* Echo maps onto the device's transmit acknowledgements, a channel setting, so the
             * channel is reopened with it changed. */
            client->config.receive_own_messages = on;

            const short result = reopen_channel(client);

            if (result != RP1210_NO_ERRORS)
            {
                return result;
            }

            {
                std::lock_guard<std::mutex> lock(client->engine_mutex);

                client->engine->set_echo(on);
            }

            client->echo = on;

            return RP1210_NO_ERRORS;
        }

        case RPCMD_FLUSH_TX_RX_BUFFERS:
        {
            client->can->flush();

            std::lock_guard<std::mutex> lock(client->engine_mutex);

            /* Drops queues and in-flight transfers, but keeps the claimed address: an application
             * clearing its buffers has not asked to leave the network. */
            client->engine->flush();

            return RP1210_NO_ERRORS;
        }

        default:
        {
            return command_not_handled;
        }
    }
}

/* Device microseconds to the 4-byte big-endian RP1210 timestamp. The INI's TimeStampWeight declares
 * the tick as 1 ms, which gives about 49 days of range instead of 72 minutes at microseconds. */
void write_timestamp(unsigned char *out, uint64_t us)
{
    const uint32_t ticks = (uint32_t)(us / 1000);

    out[0] = (unsigned char)(ticks >> 24);
    out[1] = (unsigned char)(ticks >> 16);
    out[2] = (unsigned char)(ticks >> 8);
    out[3] = (unsigned char)(ticks);
}

/* Whether the device can generate the requested bit timing. A classic channel still sends a data
 * phase, the same placeholder bus::open uses. */
bool timing_possible(const connect_params_t &params)
{
    const bool fd = params.data_bitrate != 0;
    bit_timing_t nominal;
    bit_timing_t data;

    return bit_timing_t::calculate(params.bitrate, params.sample_point,
                                   fd ? params.data_bitrate : 1000000,
                                   fd ? params.data_sample_point
                                      : bit_timing_t::default_sample_point,
                                   nominal, data);
}

} // namespace

short connect_impl(HWND hwndClient, short nDeviceID, const char *fpchProtocol,
                                  long lTxBufferSize, long lRcvBufferSize,
                                  short nIsAppPacketizingIncomingMsgs)
{
    (void)hwndClient;
    (void)lTxBufferSize;
    (void)lRcvBufferSize;


    if (fpchProtocol == nullptr)
    {
        return ERR_NULL_PARAMETER;
    }

    char name[32];

    split_protocol(fpchProtocol, name, sizeof(name));

    protocol_t protocol;

    if (protocol_is(name, "CAN"))
    {
        protocol = protocol_t::can;
    }
    else if (protocol_is(name, "J1939"))
    {
        protocol = protocol_t::j1939;
    }
    else
    {
        /* ISO15765, J1708 and the rest are refused rather than half implemented, which would fail
         * in ways that look like device faults. */
        return ERR_INVALID_PROTOCOL;
    }

    device_entry_t device;

    if (!find_device(nDeviceID, device))
    {
        return ERR_INVALID_DEVICE;
    }

    const short baud_error = (protocol == protocol_t::j1939) ? ERR_J1939_BAUD_SET_NONSTANDARD
                                                             : ERR_CAN_BAUD_SET_NONSTANDARD;

    connect_params_t params;
    std::string detail;

    switch (parse_connect_params(fpchProtocol, protocol == protocol_t::j1939, params, detail))
    {
        case params_error_t::none:
        {
            break;
        }
        case params_error_t::bad_baud:
        {
            set_connect_error(detail);

            return baud_error;
        }
        default:
        {
            /* The protocol string asks for something this protocol cannot do. */
            set_connect_error(detail);

            return ERR_INVALID_PROTOCOL;
        }
    }

    /* Refuse a bit timing the device cannot generate before touching the device. Baud=Auto picks
     * from rates known to work, so it is checked once the probe has chosen. */
    if (!params.autodetect && !timing_possible(params))
    {
        set_connect_error("the CANsub cannot generate this bit timing");

        return baud_error;
    }

    uint32_t bitrate = params.bitrate;
    const bool autodetect = params.autodetect;

    std::lock_guard<std::mutex> lock(table_mutex());

    /* Discovery happens under the lock so two connects cannot race to claim the same found device,
     * and before the in-use scan below, which compares hostnames. */
    const short resolved = resolve_device(device);

    if (resolved != RP1210_NO_ERRORS)
    {
        return resolved;
    }

    /* One client per channel, matching the device, which permits a single WebSocket per channel. */
    for (short i = 0; i < max_clients; i++)
    {
        const client_t *existing = acquire(i);

        if (existing != nullptr && existing->connected && existing->host == device.host
            && existing->channel == device.channel)
        {
            return ERR_DEVICE_IN_USE;
        }
    }

    /* The probe needs the channel, so it runs after the in-use check and before the real open. */
    if (autodetect && !probe_bitrate(device.host, device.channel, bitrate))
    {
        return ERR_CANNOT_DETERMINE_BAUD_RATE;
    }

    short id = -1;
    client_t *client = allocate(id);

    if (client == nullptr)
    {
        return ERR_CLIENT_AREA_FULL;
    }

    client->protocol = protocol;
    client->app_packetizing = (nIsAppPacketizingIncomingMsgs != 0);
    client->host = device.host;
    client->channel = device.channel;
    client->config = config_t();
    client->config.bitrate = bitrate;
    client->config.data_bitrate = params.data_bitrate;
    client->config.sample_point = params.sample_point;
    client->config.data_sample_point = params.data_sample_point;
    client->config.listen_only = params.listen_only;
    client->config.error_frames = false;
    client->config.receive_own_messages = false;
    client->echo = false;
    client->filter_exclusive = false;
    client->inclusive.clear();
    client->exclusive_ids.clear();
    client->exclusive_masks.clear();

    client->can.reset(new bus());

    const status_t result = client->can->open(device.host, device.channel, client->config);

    if (!ok(result))
    {
        /* Keep the reason before release() destroys the bus that knows it. */
        set_connect_error(client->can->last_error());
        release(id);

        switch (result)
        {
            case status_t::channel_busy:            return ERR_DEVICE_IN_USE;
            case status_t::api_version_unsupported: return ERR_DEVICE_NOT_SUPPORTED;
            case status_t::connect_failed:
            case status_t::tls_failed:              return ERR_HARDWARE_NOT_RESPONDING;
            case status_t::invalid_argument:        return ERR_CAN_BAUD_SET_NONSTANDARD;
            default:                                return ERR_HARDWARE_NOT_RESPONDING;
        }
    }

    if (protocol == protocol_t::j1939)
    {
        /* The J1939 client leaves the hardware wide open to extended identifiers and does its
         * filtering in software instead.
         *
         * It has to: the driver itself must keep seeing transport-protocol frames, address claims
         * and requests whatever the application has filtered, or a narrow filter would silently
         * break reassembly and address defence. RP1210's "nothing until a filter is set" rule is
         * honoured one level up, by refusing to *deliver* rather than refusing to receive. */
        if (!ok(client->can->set_hw_filters({filter_t::accept_all(true)})))
        {
            release(id);

            return ERR_HARDWARE_NOT_RESPONDING;
        }

        j1939::engine::config_t engine_config;

        engine_config.app_packetizing = client->app_packetizing;

        client->engine.reset(new j1939::engine(
            [client](const j1939::raw_frame_t &out) {
                frame_t frame;

                frame.id = out.id;
                frame.extended = true;
                frame.len = out.len;
                memcpy(frame.data, out.data, out.len);

                client->can->send(frame);
            },
            [] { return (uint64_t)GetTickCount64(); }, engine_config));

        client->start_worker();
    }
    else if (!ok(client->can->set_hw_filters({})))
    {
        /* RP1210: "The Client does not receive any messages until a filter is set." libcansub's
         * set_filters treats an empty list as accept-all, following python-can, so use the
         * lower-level set_hw_filters to install none and leave the device passing nothing. */
        release(id);

        return ERR_HARDWARE_NOT_RESPONDING;
    }

    client->connected = true;

    RP1210_TRACE("ClientConnect -> client %d (host %s channel %d, %s, %u bit/s at %u.%02u%%, "
                 "data %u bit/s at %u.%02u%%%s)",
                 (int)id, client->host.c_str(), client->channel,
                 (protocol == protocol_t::j1939) ? "J1939" : "CAN", client->config.bitrate,
                 client->config.sample_point / 100, client->config.sample_point % 100,
                 client->config.data_bitrate, client->config.data_sample_point / 100,
                 client->config.data_sample_point % 100,
                 client->config.listen_only ? ", listen only" : "");

    return id;
}

short disconnect_impl(short nClientID)
{
    std::lock_guard<std::mutex> lock(table_mutex());
    client_t *client = acquire(nClientID);

    if (client == nullptr || !client->connected)
    {
        return ERR_INVALID_CLIENT_ID;
    }

    release(nClientID);

    return RP1210_NO_ERRORS;
}

short send_impl(short nClientID, unsigned char *fpchClientMessage,
                                short nMessageSize, short nNotifyStatusOnTx, short nBlockOnSend)
{
    /* Both flags are ignored in RP1210E; sends are always non-blocking (section 16.3). */
    (void)nNotifyStatusOnTx;
    (void)nBlockOnSend;

    client_t *client = acquire(nClientID);

    if (client == nullptr || !client->connected)
    {
        return ERR_INVALID_CLIENT_ID;
    }

    if (fpchClientMessage == nullptr)
    {
        return ERR_NULL_PARAMETER;
    }

    if (client->protocol == protocol_t::j1939)
    {
        /* PGN (3 bytes, little endian) | How/Priority | Source | Destination | data, per RP1210E
         * section 16.7. There is no message type byte and no timestamp on the way out. */
        if (nMessageSize < 6)
        {
            return ERR_MESSAGE_NOT_SENT; /* see note on E-only codes */
        }

        const unsigned char *body = fpchClientMessage;
        const uint32_t pgn = (uint32_t)body[0] | ((uint32_t)body[1] << 8) | ((uint32_t)body[2] << 16);
        const unsigned char how = body[3];
        const unsigned char sa = body[4];
        const unsigned char da = body[5];
        const size_t length = (size_t)nMessageSize - 6;

        std::lock_guard<std::mutex> lock(client->engine_mutex);

        /* Bit 7 asks for the broadcast transport; the low three bits are the priority. */
        const j1939::result_t result = client->engine->submit(pgn, (uint8_t)(how & 0x07), sa, da,
                                                              (how & 0x80) != 0, body + 6, length);

        if (result != j1939::result_t::ok)
        {
            client->last_error = error_from(result);

            return client->last_error;
        }

        /* Pump straight away rather than waiting for the worker's next wakeup: a diagnostic tool
         * measures its own round trips, and up to 50 ms of latency per request would show. Sending
         * from here is safe because libcansub allows send and recv from different threads, and the
         * engine itself is serialised by engine_mutex. */
        client->engine->tick();

        return RP1210_NO_ERRORS;
    }

    /* Message Type plus at least a standard identifier. */
    if (nMessageSize < 3)
    {
        return ERR_MESSAGE_NOT_SENT; /* see note on E-only codes */
    }

    const unsigned char type = fpchClientMessage[0];

    if (type != RP1210_STANDARD_CAN && type != RP1210_EXTENDED_CAN)
    {
        return ERR_INVALID_COMMAND; /* see note on E-only codes */
    }

    const int id_bytes = (type == RP1210_EXTENDED_CAN) ? 4 : 2;

    if (nMessageSize < 1 + id_bytes)
    {
        return ERR_MESSAGE_NOT_SENT; /* see note on E-only codes */
    }

    const int payload = nMessageSize - 1 - id_bytes;

    if (payload > 8)
    {
        return ERR_MESSAGE_TOO_LONG;
    }

    /* Identifier is big-endian, two bytes for standard and four for extended (section 16.8). */
    uint32_t id = 0;

    for (int i = 0; i < id_bytes; i++)
    {
        id = (id << 8) | fpchClientMessage[1 + i];
    }

    frame_t frame;

    frame.id = id & ((type == RP1210_EXTENDED_CAN) ? 0x1FFFFFFFu : 0x7FFu);
    frame.extended = (type == RP1210_EXTENDED_CAN);
    frame.len = (uint8_t)payload;
    memcpy(frame.data, fpchClientMessage + 1 + id_bytes, (size_t)payload);

    const status_t result = client->can->send(frame);

    if (!ok(result))
    {
        client->last_error = (result == status_t::disconnected) ? ERR_CLIENT_DISCONNECTED
                                                               : ERR_MESSAGE_NOT_SENT;

        return client->last_error;
    }

    return RP1210_NO_ERRORS;
}

short read_impl(short nClientID, unsigned char *fpchAPIMessage, short nBufferSize,
                               short nBlockOnRead)
{
    client_t *client = acquire(nClientID);

    if (client == nullptr || !client->connected)
    {
        return -ERR_INVALID_CLIENT_ID;
    }

    if (fpchAPIMessage == nullptr)
    {
        return -ERR_NULL_PARAMETER;
    }

    const int timeout = (nBlockOnRead == RP1210_BLOCKING_IO) ? client->block_timeout_ms : 0;

    if (client->protocol == protocol_t::j1939)
    {
        std::unique_lock<std::mutex> lock(client->engine_mutex);

        if (client->engine->pending() == 0 && timeout > 0)
        {
            client->rx_wake.wait_for(lock, std::chrono::milliseconds(timeout), [client] {
                return client->engine->pending() > 0 || client->worker_stop.load();
            });
        }

        const j1939::message_t *message = client->engine->front();

        if (message == nullptr)
        {
            return 0;
        }

        /* Timestamp, the echo byte only when echo mode is on, then PGN (little endian),
         * How/Priority, source, destination, data (RP1210E section 17.6). */
        const int needed = 4 + (client->echo ? 1 : 0) + 3 + 1 + 1 + 1 + (int)message->data.size();

        if (nBufferSize < needed)
        {
            /* The message stays queued: an application that retries with a bigger buffer gets it,
             * rather than losing it to having guessed the size wrong. */
            return -ERR_MESSAGE_TOO_LONG;
        }

        int pos = 0;

        write_timestamp(fpchAPIMessage, message->us);
        pos += 4;

        if (client->echo)
        {
            fpchAPIMessage[pos++] = message->echo ? 0x01 : 0x00;
        }

        fpchAPIMessage[pos++] = (unsigned char)(message->pgn & 0xFF);
        fpchAPIMessage[pos++] = (unsigned char)((message->pgn >> 8) & 0xFF);
        fpchAPIMessage[pos++] = (unsigned char)((message->pgn >> 16) & 0xFF);

        /* Bit 7 reports which transport carried it; the low three bits are the priority. */
        fpchAPIMessage[pos++] =
            (unsigned char)((message->bam ? 0x80 : 0x00) | (message->priority & 0x07));

        fpchAPIMessage[pos++] = message->sa;
        fpchAPIMessage[pos++] = message->da;

        if (!message->data.empty())
        {
            memcpy(fpchAPIMessage + pos, message->data.data(), message->data.size());
            pos += (int)message->data.size();
        }

        client->engine->drop_front();

        return (short)pos;
    }

    std::lock_guard<std::mutex> read_lock(client->read_mutex);

    /* An exclusive filter is applied here, so keep reading until something passes it. */
    for (;;)
    {
        frame_t frame;

        if (client->held)
        {
            frame = client->held_frame;
            client->held = false;
        }
        else
        {
            const status_t result = client->can->recv(frame, timeout);

            if (result == status_t::timeout)
            {
                return 0;
            }

            if (result == status_t::disconnected)
            {
                return -ERR_CLIENT_DISCONNECTED;
            }

            if (!ok(result))
            {
                return -ERR_RX_QUEUE_CORRUPT;
            }
        }

        /* Error frames have no RP1210 CAN representation, so they are counted and skipped rather
         * than fabricated into a data frame. GetHardwareStatus reports the bus state instead. */
        if (frame.is_error)
        {
            continue;
        }

        /* Neither do CAN FD frames: the RP1210 CAN message has no FD or bit-rate-switch flag and
         * at most 8 data bytes, so passing one on would misreport it as a classic frame. The device
         * always runs in FD mode, so they can arrive on any channel. CANSUB_ReadXnetFrames
         * delivers them. */
        if (frame.fd)
        {
            if (client->fd_skipped++ == 0)
            {
                RP1210_TRACE("ReadMessage: client %d skips CAN FD frames, which RP1210 cannot "
                             "represent (CANSUB_ReadXnetFrames delivers them)",
                             (int)nClientID);
            }

            continue;
        }

        if (blocked(*client, frame))
        {
            continue;
        }

        /* Timestamp, then the echo byte only when echo mode is on, then type, identifier, data
         * (section 17.7). The echo byte's presence changes the layout of every read. */
        const int id_bytes = frame.extended ? 4 : 2;
        const int needed = 4 + (client->echo ? 1 : 0) + 1 + id_bytes + frame.len;

        if (nBufferSize < needed)
        {
            /* The frame stays for the next read, as on the J1939 path: an application that
             * retries with a bigger buffer gets it, rather than losing it. */
            client->held_frame = frame;
            client->held = true;

            return -ERR_MESSAGE_TOO_LONG;
        }

        int pos = 0;

        write_timestamp(fpchAPIMessage, frame.us);
        pos += 4;

        if (client->echo)
        {
            fpchAPIMessage[pos++] = frame.tx ? 0x01 : 0x00;
        }

        fpchAPIMessage[pos++] = frame.extended ? RP1210_EXTENDED_CAN : RP1210_STANDARD_CAN;

        for (int i = id_bytes - 1; i >= 0; i--)
        {
            fpchAPIMessage[pos++] = (unsigned char)(frame.id >> (i * 8));
        }

        memcpy(fpchAPIMessage + pos, frame.data, frame.len);
        pos += frame.len;

        return (short)pos;
    }
}

/* ---------------------------------------------------------------- NI-XNET raw frames
 *
 * CANSUB_ReadXnetFrames, the LabVIEW extension declared in rp1210.h. It reads through the same
 * client, bus and exclusive filter as RP1210_ReadMessage, and only the output layout differs, so
 * nothing on the RP1210 path is touched.
 *
 * NI-XNET raw frame format, little endian: a 24-byte base unit of timestamp (U64, 100 ns since
 * 1601-01-01 UTC), identifier (U32, bit 29 set for extended), type, flags, info, payload length and
 * 8 payload bytes, then for a payload over 8 bytes the rest padded to a multiple of 8. */
namespace
{

constexpr uint32_t xnet_id_extended = 0x20000000u;
constexpr unsigned char xnet_type_can_data = 0x00;
constexpr unsigned char xnet_type_can_remote = 0x01;
constexpr unsigned char xnet_type_canfd_data = 0x10;
constexpr unsigned char xnet_type_canfdbrs_data = 0x18;
constexpr unsigned char xnet_flag_transmit_echo = 0x80;

/* 100 ns intervals from 1601-01-01 to 1970-01-01, the Windows FILETIME epoch NI-XNET uses. */
constexpr uint64_t xnet_epoch_offset = 116444736000000000ull;

void put_le(unsigned char *out, uint64_t value, int bytes)
{
    for (int i = 0; i < bytes; i++)
    {
        out[i] = (unsigned char)(value >> (i * 8));
    }
}

/* Writes one frame and returns its size, which is never more than CANSUB_XNET_FRAME_MAX. */
long write_xnet_frame(unsigned char *out, const frame_t &frame)
{
    const int extra = (frame.len > 8) ? ((frame.len - 8 + 7) & ~7) : 0;
    const long size = 24 + extra;

    memset(out, 0, (size_t)size);

    put_le(out, frame.us * 10 + xnet_epoch_offset, 8);
    put_le(out + 8, frame.extended ? (frame.id | xnet_id_extended) : frame.id, 4);

    out[12] = frame.fd    ? (frame.brs ? xnet_type_canfdbrs_data : xnet_type_canfd_data)
              : frame.rtr ? xnet_type_can_remote
                          : xnet_type_can_data;
    out[13] = frame.tx ? xnet_flag_transmit_echo : 0x00;
    out[15] = frame.len;

    /* The base unit holds the first 8 payload bytes, the payload unit the rest. */
    memcpy(out + 16, frame.data, (frame.len > 8) ? 8 : frame.len);

    if (frame.len > 8)
    {
        memcpy(out + 24, frame.data + 8, (size_t)(frame.len - 8));
    }

    return size;
}

uint64_t get_le(const unsigned char *in, int bytes)
{
    uint64_t value = 0;

    for (int i = bytes - 1; i >= 0; i--)
    {
        value = (value << 8) | in[i];
    }

    return value;
}

/* Whether len is a payload length CAN FD can carry: 0-8, 12, 16, 20, 24, 32, 48 or 64. */
bool is_fd_length(uint8_t len)
{
    switch (len)
    {
        case 12: case 16: case 20: case 24: case 32: case 48: case 64:
        {
            return true;
        }
        default:
        {
            return len <= 8;
        }
    }
}

/* Reads one frame for sending, as XNET Convert (Signal to Frame Raw) produces them. Returns its size
 * in the buffer, or 0 if it is malformed. Whether the channel can send it (an FD frame needs a data
 * bit rate) is the caller's check. The timestamp and flags of an outgoing frame mean nothing to the
 * device and are ignored. */
long read_xnet_frame(const unsigned char *in, long available, frame_t &frame)
{
    if (available < 24)
    {
        return 0;
    }

    const unsigned char type = in[12];
    const uint8_t len = in[15];
    const int extra = (len > 8) ? ((len - 8 + 7) & ~7) : 0;
    const long size = 24 + extra;

    if (size > available)
    {
        return 0;
    }

    /* Other NI-XNET frame types (delays, triggers, bus errors) are not CAN frames at all. */
    const bool fd = (type == xnet_type_canfd_data || type == xnet_type_canfdbrs_data);

    if (fd ? !is_fd_length(len)
           : ((type != xnet_type_can_data && type != xnet_type_can_remote) || len > 8))
    {
        return 0;
    }

    const uint32_t id = (uint32_t)get_le(in + 8, 4);

    frame = frame_t{};
    frame.extended = (id & xnet_id_extended) != 0;
    frame.id = id & (frame.extended ? 0x1FFFFFFFu : 0x7FFu);
    frame.fd = fd;
    frame.brs = (type == xnet_type_canfdbrs_data);
    frame.rtr = (type == xnet_type_can_remote);
    frame.len = len;

    if (!frame.rtr)
    {
        memcpy(frame.data, in + 16, len);
    }

    return size;
}

} // namespace

/* CANSUB_WriteXnetFrames, the mirror of CANSUB_ReadXnetFrames: sends frames given in NI-XNET's raw
 * format, so LabVIEW can send by DBC signal name via XNET Convert (Signal to Frame Raw). It sends
 * through the same client and bus as RP1210_SendMessage, so echo mode applies as there.
 *
 * The whole buffer is checked before anything is sent, so a malformed buffer sends nothing. A send
 * that fails part-way returns the error, with the frames already sent in *pFrameCount. */
long write_xnet_impl(short nClientID, const unsigned char *pBuffer, long nBufferSize, long *pFrameCount)
{
    if (pFrameCount != nullptr)
    {
        *pFrameCount = 0;
    }

    client_t *client = acquire(nClientID);

    if (client == nullptr || !client->connected)
    {
        return -ERR_INVALID_CLIENT_ID;
    }

    if (pBuffer == nullptr && nBufferSize > 0)
    {
        return -ERR_NULL_PARAMETER;
    }

    /* J1939 messages go through the engine, which needs the RP1210 J1939 layout. */
    if (client->protocol != protocol_t::can)
    {
        return -ERR_INVALID_PROTOCOL;
    }

    std::vector<frame_t> frames;

    for (long pos = 0; pos < nBufferSize;)
    {
        frame_t frame;
        const long size = read_xnet_frame(pBuffer + pos, nBufferSize - pos, frame);

        if (size == 0)
        {
            return -ERR_MESSAGE_NOT_SENT; /* see note on E-only codes */
        }

        /* Refused up front, so the buffer is sent whole or not at all: a listen-only channel
         * sends nothing, and an FD frame needs a channel connected with DataBaud=. */
        if (client->config.listen_only || (frame.fd && client->config.data_bitrate == 0))
        {
            return -ERR_MESSAGE_NOT_SENT; /* see note on E-only codes */
        }

        frames.push_back(frame);
        pos += size;
    }

    long sent = 0;

    for (const frame_t &frame : frames)
    {
        const status_t result = client->can->send(frame);

        if (!ok(result))
        {
            client->last_error = (result == status_t::disconnected) ? ERR_CLIENT_DISCONNECTED
                                                                   : ERR_MESSAGE_NOT_SENT;

            if (pFrameCount != nullptr)
            {
                *pFrameCount = sent;
            }

            return -client->last_error;
        }

        sent++;
    }

    if (pFrameCount != nullptr)
    {
        *pFrameCount = sent;
    }

    return sent;
}

long read_xnet_impl(short nClientID, unsigned char *pBuffer, long nBufferSize, long *pFrameCount)
{
    if (pFrameCount != nullptr)
    {
        *pFrameCount = 0;
    }

    client_t *client = acquire(nClientID);

    if (client == nullptr || !client->connected)
    {
        return -ERR_INVALID_CLIENT_ID;
    }

    if (pBuffer == nullptr)
    {
        return -ERR_NULL_PARAMETER;
    }

    /* J1939 messages are reassembled by the engine and have no single CAN frame to report. */
    if (client->protocol != protocol_t::can)
    {
        return -ERR_INVALID_PROTOCOL;
    }

    if (nBufferSize < CANSUB_XNET_FRAME_MAX)
    {
        return -ERR_MESSAGE_TOO_LONG;
    }

    std::lock_guard<std::mutex> read_lock(client->read_mutex);

    long pos = 0;
    long count = 0;

    /* A received frame cannot be put back, so read only while the largest one would still fit. */
    while (nBufferSize - pos >= CANSUB_XNET_FRAME_MAX)
    {
        frame_t frame;

        /* A frame RP1210_ReadMessage held back for a bigger buffer comes first. */
        if (client->held)
        {
            frame = client->held_frame;
            client->held = false;
        }
        else
        {
            const status_t result = client->can->recv(frame, 0);

            if (result == status_t::timeout)
            {
                break;
            }

            /* Frames already written are delivered first; the next call reports the failure. */
            if (!ok(result))
            {
                if (pos > 0)
                {
                    break;
                }

                return (result == status_t::disconnected) ? -ERR_CLIENT_DISCONNECTED
                                                          : -ERR_RX_QUEUE_CORRUPT;
            }
        }

        if (frame.is_error || blocked(*client, frame))
        {
            continue;
        }

        pos += write_xnet_frame(pBuffer + pos, frame);
        count++;
    }

    if (pFrameCount != nullptr)
    {
        *pFrameCount = count;
    }

    return pos;
}

short command_impl(short nCommandNumber, short nClientID,
                                const char *fpchClientCommand, short nMessageSize)
{
    client_t *client = acquire(nClientID);

    if (client == nullptr || !client->connected)
    {
        return ERR_INVALID_CLIENT_ID;
    }

    const bool j1939_client = (client->protocol == protocol_t::j1939);

    /* Several commands belong to one protocol only. Refusing them on the other one is not
     * pedantry: an application that sets a CAN identifier filter on a J1939 client would otherwise
     * believe it had filtered something. */
    switch (nCommandNumber)
    {
        case RPCMD_SET_MESSAGE_FILTERING_FOR_CAN:
        case RPCMD_SET_CAN_FILTER_TYPE:
        case RPCMD_SET_CAN_BAUD:
        {
            if (j1939_client)
            {
                return ERR_COMMAND_NOT_SUPPORTED;
            }

            break;
        }

        case RPCMD_SET_MESSAGE_FILTERING_FOR_J1939:
        case RPCMD_PROTECT_J1939_ADDRESS:
        case RPCMD_SET_BROADCAST_FOR_J1939:
        case RPCMD_SET_J1939_FILTER_TYPE:
        case RPCMD_SET_J1939_INTERPACKET_TIME:
        case RPCMD_RELEASE_J1939_ADDRESS:
        case RPCMD_SET_J1939_BAUD:
        {
            if (!j1939_client)
            {
                return ERR_COMMAND_NOT_SUPPORTED;
            }

            break;
        }

        default:
        {
            break;
        }
    }

    if (j1939_client)
    {
        const short handled = j1939_command(client, nCommandNumber, fpchClientCommand, nMessageSize);

        if (handled != command_not_handled)
        {
            return handled;
        }
    }

    switch (nCommandNumber)
    {
        case RPCMD_RESET_DEVICE:
        {
            /* Reopening the channel is the closest equivalent, and it clears the CAN error state. */
            client->can->close();

            return ok(client->can->open(client->host, client->channel, client->config))
                       ? RP1210_NO_ERRORS
                       : ERR_HARDWARE_NOT_RESPONDING;
        }

        case RPCMD_SET_ALL_FILTERS_STATES_TO_PASS:
        {
            client->filter_exclusive = false;
            client->inclusive.clear();
            client->exclusive_ids.clear();
            client->exclusive_masks.clear();

            return ok(client->can->set_filters({})) ? RP1210_NO_ERRORS : ERR_HARDWARE_NOT_RESPONDING;
        }

        case RPCMD_SET_ALL_FILTERS_STATES_TO_DISCARD:
        {
            client->filter_exclusive = false;
            client->inclusive.clear();
            client->exclusive_ids.clear();
            client->exclusive_masks.clear();

            /* No filters installed is the device's own way to pass nothing. */
            return ok(client->can->set_hw_filters({})) ? RP1210_NO_ERRORS
                                                       : ERR_HARDWARE_NOT_RESPONDING;
        }

        case RPCMD_SET_MESSAGE_FILTERING_FOR_CAN:
        {
            if (fpchClientCommand == nullptr || nMessageSize < 3)
            {
                return ERR_INVALID_COMMAND; /* see note on E-only codes */
            }

            /* RP1210 defines this as nine bytes: message type, then a four-byte mask saying which
             * bits matter, then a four-byte header giving the values those bits must have. Both are
             * big-endian.
             *
             * Two shorter forms are also accepted, because an application that only ever wants one
             * identifier tends to send just that: type plus a two- or four-byte identifier, which
             * is the layout a message itself uses. Reading the length is what tells them apart --
             * and getting this wrong is expensive, because interpreting a nine-byte command as the
             * short form reads the mask as an identifier and filters against nonsense. */
            const unsigned char type = (unsigned char)fpchClientCommand[0];
            const bool extended = (type == RP1210_EXTENDED_CAN);
            const uint32_t id_mask = extended ? 0x1FFFFFFFu : 0x7FFu;

            uint32_t mask = 0;
            uint32_t header = 0;

            if (nMessageSize >= 9)
            {
                for (int i = 0; i < 4; i++)
                {
                    mask = (mask << 8) | (unsigned char)fpchClientCommand[1 + i];
                    header = (header << 8) | (unsigned char)fpchClientCommand[5 + i];
                }

                mask &= id_mask;
                header &= id_mask;
            }
            else
            {
                const int id_bytes = extended ? 4 : 2;

                if (nMessageSize < 1 + id_bytes)
                {
                    return ERR_INVALID_COMMAND; /* see note on E-only codes */
                }

                for (int i = 0; i < id_bytes; i++)
                {
                    header = (header << 8) | (unsigned char)fpchClientCommand[1 + i];
                }

                header &= id_mask;
                mask = id_mask; /* every bit matters: this is one exact identifier */
            }

            if (client->filter_exclusive)
            {
                /* The exclusive list is judged in software, so it carries the mask with it. */
                client->exclusive_ids.push_back(header);
                client->exclusive_masks.push_back(mask);

                return RP1210_NO_ERRORS;
            }

            /* Applications set filters one entry at a time, so accumulate rather than replace, and
             * reinstall the whole set. A CANsub hardware filter is an identifier and a mask, which
             * is exactly what RP1210 asks for here. */
            filter_t filter;

            filter.extended = extended;
            filter.range = false;
            filter.f1 = header;
            filter.f2 = mask;

            client->inclusive.push_back(filter);

            return ok(client->can->set_hw_filters(client->inclusive)) ? RP1210_NO_ERRORS
                                                                     : ERR_MAX_FILTERS_EXCEEDED;
        }

        case RPCMD_SET_CAN_FILTER_TYPE:
        {
            if (fpchClientCommand == nullptr || nMessageSize < 1)
            {
                return ERR_INVALID_COMMAND; /* see note on E-only codes */
            }

            client->filter_exclusive = (fpchClientCommand[0] == RP1210_FILTER_EXCLUSIVE);
            client->inclusive.clear();
            client->exclusive_ids.clear();
            client->exclusive_masks.clear();

            /* An exclusive filter needs everything through the hardware so it can be judged here. */
            if (client->filter_exclusive && !ok(client->can->set_filters({})))
            {
                return ERR_HARDWARE_NOT_RESPONDING;
            }

            return RP1210_NO_ERRORS;
        }

        case RPCMD_ECHO_TRANSMITTED_MESSAGES:
        {
            if (fpchClientCommand == nullptr || nMessageSize < 1)
            {
                return ERR_INVALID_COMMAND; /* see note on E-only codes */
            }

            const bool on = (fpchClientCommand[0] == RP1210_ECHO_ON);

            if (on == client->echo)
            {
                return RP1210_NO_ERRORS;
            }

            /* Echo maps onto the device's tx_ack_frames, which is a channel setting, so the channel
             * is reopened with it changed. */
            client->config.receive_own_messages = on;
            client->can->close();

            if (!ok(client->can->open(client->host, client->channel, client->config)))
            {
                return ERR_HARDWARE_NOT_RESPONDING;
            }

            client->echo = on;

            return RP1210_NO_ERRORS;
        }

        case RPCMD_SET_BLOCKTIMEOUT:
        {
            if (fpchClientCommand == nullptr || nMessageSize < 2)
            {
                return ERR_INVALID_COMMAND; /* see note on E-only codes */
            }

            /* Two bytes, big-endian, in milliseconds. */
            client->block_timeout_ms = ((unsigned char)fpchClientCommand[0] << 8)
                                       | (unsigned char)fpchClientCommand[1];

            return RP1210_NO_ERRORS;
        }

        case RPCMD_FLUSH_TX_RX_BUFFERS:
        {
            client->can->flush();

            /* A read blocked on another thread owns the held frame; it is not waited for. */
            std::unique_lock<std::mutex> read_lock(client->read_mutex, std::try_to_lock);

            if (read_lock.owns_lock())
            {
                client->held = false;
            }

            return RP1210_NO_ERRORS;
        }

        case RPCMD_GET_PROTOCOL_CONNECTION_SPEED:
        {
            if (fpchClientCommand == nullptr || nMessageSize < 17)
            {
                return ERR_INVALID_COMMAND; /* see note on E-only codes */
            }

            char text[20] = {};

            snprintf(text, sizeof(text), "%u", client->config.bitrate / 1000);
            strncpy_s(const_cast<char *>(fpchClientCommand), (size_t)nMessageSize, text,
                      (size_t)nMessageSize - 1);

            return RP1210_NO_ERRORS;
        }

        case RPCMD_SET_CAN_BAUD:
        {
            if (fpchClientCommand == nullptr || nMessageSize < 2)
            {
                return ERR_INVALID_COMMAND; /* see note on E-only codes */
            }

            /* Byte 0 selects when to change, byte 1 the baud identifier. */
            const uint32_t bitrate = bitrate_from_baud_id(fpchClientCommand[1]);

            if (bitrate == 0)
            {
                return ERR_CAN_BAUD_SET_NONSTANDARD;
            }

            /* Check first: a rate the device cannot generate with the client's other timing
             * settings (a data rate below it, say) must not cost the application its channel. */
            connect_params_t params;

            params.bitrate = bitrate;
            params.sample_point = client->config.sample_point;
            params.data_bitrate = client->config.data_bitrate;
            params.data_sample_point = client->config.data_sample_point;

            if (!timing_possible(params))
            {
                return ERR_CAN_BAUD_SET_NONSTANDARD;
            }

            client->config.bitrate = bitrate;
            client->can->close();

            return ok(client->can->open(client->host, client->channel, client->config))
                       ? RP1210_NO_ERRORS
                       : ERR_CANNOT_SET_CAN_BAUDRATE;
        }

        default:
        {
            return ERR_COMMAND_NOT_SUPPORTED;
        }
    }
}

void WINAPI RP1210_ReadVersion(char *fpchDLLMajorVersion, char *fpchDLLMinorVersion,
                               char *fpchAPIMajorVersion, char *fpchAPIMinorVersion)
{
    /* RP1210 documents these as at least two bytes each. */
    copy_fixed(fpchDLLMajorVersion, dll_major, 2);
    copy_fixed(fpchDLLMinorVersion, dll_minor, 2);
    copy_fixed(fpchAPIMajorVersion, api_major, 2);
    copy_fixed(fpchAPIMinorVersion, api_minor, 2);
}

short WINAPI RP1210_ReadDetailedVersion(short nClientID, char *fpchAPIVersionInfo,
                                        char *fpchDLLVersionInfo, char *fpchFWVersionInfo)
{
    client_t *client = acquire(nClientID);

    if (client == nullptr || !client->connected)
    {
        return ERR_INVALID_CLIENT_ID;
    }

    /* RP1210 specifies 17-byte buffers for these. */
    copy_fixed(fpchAPIVersionInfo, api_revision, 17);
    copy_fixed(fpchDLLVersionInfo, dll_version_info, 17);
    copy_fixed(fpchFWVersionInfo, client->can->firmware_version().c_str(), 17);

    return RP1210_NO_ERRORS;
}

short WINAPI RP1210_GetErrorMsg(short err_code, char *fpchDescription)
{
    if (fpchDescription == nullptr)
    {
        return ERR_NULL_PARAMETER;
    }

    if (err_code == RP1210_NO_ERRORS)
    {
        copy_fixed(fpchDescription, "NO_ERRORS", RP1210_MAX_ERR_MSG_LENGTH);

        return RP1210_NO_ERRORS;
    }

    for (int i = 0; i < RP1210_ERROR_COUNT; i++)
    {
        if (rp1210_error_texts[i].code == err_code)
        {
            copy_fixed(fpchDescription, rp1210_error_texts[i].text, RP1210_MAX_ERR_MSG_LENGTH);

            return RP1210_NO_ERRORS;
        }
    }

    return ERR_CODE_NOT_FOUND;
}

short WINAPI RP1210_GetLastErrorMsg(short err_code, int *SubErrorCode, char *fpchDescription,
                                    short nClientID)
{
    client_t *client = acquire(nClientID);

    if (SubErrorCode != nullptr)
    {
        *SubErrorCode = 0;
    }

    /* The library's own diagnostic text is more useful than the mnemonic when there is one. */
    if (client != nullptr && client->connected && fpchDescription != nullptr)
    {
        const std::string detail = client->can->last_error();

        if (!detail.empty())
        {
            copy_fixed(fpchDescription, detail.c_str(), RP1210_MAX_ERR_MSG_LENGTH);

            return RP1210_NO_ERRORS;
        }
    }

    /* No client, which is the normal case after a failed connect: the caller has an error code and
     * no handle to ask about. Report why the connect failed instead of just naming the code. */
    if (client == nullptr && fpchDescription != nullptr)
    {
        std::lock_guard<std::mutex> lock(connect_error_mutex());

        if (!connect_error().empty())
        {
            copy_fixed(fpchDescription, connect_error().c_str(), RP1210_MAX_ERR_MSG_LENGTH);

            return RP1210_NO_ERRORS;
        }
    }

    return RP1210_GetErrorMsg(err_code, fpchDescription);
}

short hardware_status_impl(short nClientID, char *fpchClientInfo, short nInfoSize,
                                      short nBlockOnRequest)
{
    (void)nBlockOnRequest;

    client_t *client = acquire(nClientID);

    if (client == nullptr || !client->connected)
    {
        return ERR_INVALID_CLIENT_ID;
    }

    if (fpchClientInfo == nullptr)
    {
        return ERR_NULL_PARAMETER;
    }

    /* RP1210 wants at least 18 bytes here. Only the first two are meaningful for CAN: whether the
     * hardware is present and whether a client is connected. */
    if (nInfoSize < 2)
    {
        return ERR_INVALID_COMMAND; /* see note on E-only codes */
    }

    memset(fpchClientInfo, 0, (size_t)nInfoSize);

    fpchClientInfo[0] = client->can->is_open() ? 0x01 : 0x00;
    fpchClientInfo[1] = 0x01;

    return RP1210_NO_ERRORS;
}

short WINAPI RP1210_Ioctl(short nClientID, long nIoctlID, void *pInput, void *pOutput)
{
    (void)nClientID;
    (void)nIoctlID;
    (void)pInput;
    (void)pOutput;

    return ERR_INVALID_IOCTL_ID;
}

/* ---------------------------------------------------------------- traced entry points
 *
 * The exported functions are thin wrappers that record the call and its result. A diagnostic
 * application reports nothing more useful than "gateway not responding", so without this the only
 * way to find out which call it was unhappy with is to guess. Tracing is off unless DebugFile and
 * DebugLevel are set in the INI, or CANSUB_RP1210_LOG points somewhere writable.
 *
 * The real work stays in the *_impl functions above, unchanged, so tracing cannot alter behaviour. */

short WINAPI RP1210_ClientConnect(HWND hwndClient, short nDeviceID, const char *fpchProtocol,
                                  long lTxBufferSize, long lRcvBufferSize,
                                  short nIsAppPacketizingIncomingMsgs)
{
    RP1210_TRACE("ClientConnect(hwnd=%p, device=%d, protocol=\"%s\", tx=%ld, rx=%ld, appPack=%d)",
                 (void *)hwndClient, (int)nDeviceID,
                 (fpchProtocol != nullptr) ? fpchProtocol : "(null)", lTxBufferSize, lRcvBufferSize,
                 (int)nIsAppPacketizingIncomingMsgs);

    const short result = connect_impl(hwndClient, nDeviceID, fpchProtocol, lTxBufferSize,
                                      lRcvBufferSize, nIsAppPacketizingIncomingMsgs);

    if (result >= 0 && result <= 127)
    {
        RP1210_TRACE("  = client %d", (int)result);
    }
    else
    {
        char text[RP1210_MAX_ERR_MSG_LENGTH] = {};

        RP1210_GetErrorMsg(result, text);
        RP1210_TRACE("  = FAILED %d %s", (int)result, text);

        std::lock_guard<std::mutex> lock(connect_error_mutex());

        if (!connect_error().empty())
        {
            RP1210_TRACE("    detail: %s", connect_error().c_str());
        }
    }

    return result;
}

short WINAPI RP1210_ClientDisconnect(short nClientID)
{
    RP1210_TRACE("ClientDisconnect(client=%d)", (int)nClientID);

    const short result = disconnect_impl(nClientID);

    RP1210_TRACE("  = %d", (int)result);

    return result;
}

short WINAPI RP1210_SendMessage(short nClientID, unsigned char *fpchClientMessage,
                                short nMessageSize, short nNotifyStatusOnTx, short nBlockOnSend)
{
    if (trace::enabled())
    {
        if (trace::level() >= 2)
        {
            RP1210_TRACE("SendMessage(client=%d, size=%d) %s", (int)nClientID, (int)nMessageSize,
                         trace::hex(fpchClientMessage, (size_t)(nMessageSize > 0 ? nMessageSize : 0))
                             .c_str());
        }
        else
        {
            RP1210_TRACE("SendMessage(client=%d, size=%d)", (int)nClientID, (int)nMessageSize);
        }
    }

    const short result = send_impl(nClientID, fpchClientMessage, nMessageSize, nNotifyStatusOnTx,
                                   nBlockOnSend);

    if (result != RP1210_NO_ERRORS)
    {
        char text[RP1210_MAX_ERR_MSG_LENGTH] = {};

        RP1210_GetErrorMsg(result, text);
        RP1210_TRACE("  = FAILED %d %s", (int)result, text);
    }
    else
    {
        RP1210_TRACE("  = ok");
    }

    return result;
}

short WINAPI RP1210_ReadMessage(short nClientID, unsigned char *fpchAPIMessage, short nBufferSize,
                                short nBlockOnRead)
{
    const short result = read_impl(nClientID, fpchAPIMessage, nBufferSize, nBlockOnRead);

    /* A poll that finds nothing is by far the most common call an application makes, and logging
     * every one of them buries everything else. Only report reads that produced something. */
    if (trace::enabled() && result != 0)
    {
        if (result > 0)
        {
            if (trace::level() >= 2)
            {
                RP1210_TRACE("ReadMessage(client=%d, buffer=%d, block=%d) = %d bytes %s",
                             (int)nClientID, (int)nBufferSize, (int)nBlockOnRead, (int)result,
                             trace::hex(fpchAPIMessage, (size_t)result).c_str());
            }
            else
            {
                RP1210_TRACE("ReadMessage(client=%d) = %d bytes", (int)nClientID, (int)result);
            }
        }
        else
        {
            char text[RP1210_MAX_ERR_MSG_LENGTH] = {};

            RP1210_GetErrorMsg((short)-result, text);
            RP1210_TRACE("ReadMessage(client=%d, buffer=%d, block=%d) = FAILED %d %s",
                         (int)nClientID, (int)nBufferSize, (int)nBlockOnRead, (int)result, text);
        }
    }

    return result;
}

short WINAPI RP1210_SendCommand(short nCommandNumber, short nClientID,
                                const char *fpchClientCommand, short nMessageSize)
{
    if (trace::enabled())
    {
        RP1210_TRACE("SendCommand(%d, client=%d, size=%d) %s", (int)nCommandNumber, (int)nClientID,
                     (int)nMessageSize,
                     (trace::level() >= 2)
                         ? trace::hex(fpchClientCommand,
                                      (size_t)(nMessageSize > 0 ? nMessageSize : 0))
                               .c_str()
                         : "");
    }

    const short result = command_impl(nCommandNumber, nClientID, fpchClientCommand, nMessageSize);

    if (result != RP1210_NO_ERRORS)
    {
        char text[RP1210_MAX_ERR_MSG_LENGTH] = {};

        RP1210_GetErrorMsg(result, text);
        RP1210_TRACE("  = FAILED %d %s", (int)result, text);
    }
    else
    {
        RP1210_TRACE("  = ok");
    }

    return result;
}

short WINAPI RP1210_GetHardwareStatus(short nClientID, char *fpchClientInfo, short nInfoSize,
                                      short nBlockOnRequest)
{
    const short result = hardware_status_impl(nClientID, fpchClientInfo, nInfoSize,
                                              nBlockOnRequest);

    RP1210_TRACE("GetHardwareStatus(client=%d, size=%d) = %d %s", (int)nClientID, (int)nInfoSize,
                 (int)result,
                 (result == RP1210_NO_ERRORS)
                     ? trace::hex(fpchClientInfo, (size_t)(nInfoSize > 0 ? nInfoSize : 0)).c_str()
                     : "");

    return result;
}

long WINAPI CANSUB_ReadXnetFrames(short nClientID, unsigned char *pBuffer, long nBufferSize,
                                  long *pFrameCount)
{
    const long result = read_xnet_impl(nClientID, pBuffer, nBufferSize, pFrameCount);

    /* Polled like ReadMessage, so empty reads are not logged either. */
    if (trace::enabled() && result != 0)
    {
        if (result > 0)
        {
            RP1210_TRACE("CANSUB_ReadXnetFrames(client=%d, buffer=%ld) = %ld bytes, %ld frames",
                         (int)nClientID, nBufferSize, result,
                         (pFrameCount != nullptr) ? *pFrameCount : -1L);
        }
        else
        {
            char text[RP1210_MAX_ERR_MSG_LENGTH] = {};

            RP1210_GetErrorMsg((short)-result, text);
            RP1210_TRACE("CANSUB_ReadXnetFrames(client=%d, buffer=%ld) = FAILED %ld %s",
                         (int)nClientID, nBufferSize, result, text);
        }
    }

    return result;
}

long WINAPI CANSUB_WriteXnetFrames(short nClientID, const unsigned char *pBuffer, long nBufferSize,
                                   long *pFrameCount)
{
    const long result = write_xnet_impl(nClientID, pBuffer, nBufferSize, pFrameCount);

    if (trace::enabled())
    {
        if (result >= 0)
        {
            RP1210_TRACE("CANSUB_WriteXnetFrames(client=%d, size=%ld) = %ld frames %s", (int)nClientID,
                         nBufferSize, result,
                         trace::hex(pBuffer, (size_t)(nBufferSize > 0 ? nBufferSize : 0)).c_str());
        }
        else
        {
            char text[RP1210_MAX_ERR_MSG_LENGTH] = {};

            RP1210_GetErrorMsg((short)-result, text);
            RP1210_TRACE("CANSUB_WriteXnetFrames(client=%d, size=%ld) = FAILED %ld %s", (int)nClientID,
                         nBufferSize, result, text);
        }
    }

    return result;
}
