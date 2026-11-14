/* Does the RP1210 driver connect to the CANsub? Nothing more.
 *
 * Loads CANSUB32.dll the way a diagnostic application does, by name through the normal DLL search
 * path, then connects, reports what the device said, and disconnects. Use it to separate "the driver
 * and device work" from "the diagnostic application is configured correctly".
 *
 * Ask for J1939 and it goes further: it claims the offboard diagnostic tool address, asks every
 * node on the bus to identify itself, and prints what answers for five seconds. That is the quickest
 * way to tell a working J1939 setup from a quiet or mis-wired one.
 *
 * Usage: rp1210_connect [DeviceID] [protocol]
 *        rp1210_connect                     -> DeviceID 1, CAN:Baud=250
 *        rp1210_connect 2 CAN:Baud=500
 *        rp1210_connect 1 J1939:Baud=250    -> claim an address and listen
 *        rp1210_connect 1 J1939:Baud=Auto   -> detect the bit rate first */

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{

typedef short(WINAPI *connect_fn)(HWND, short, const char *, long, long, short);
typedef short(WINAPI *disconnect_fn)(short);
typedef void(WINAPI *read_version_fn)(char *, char *, char *, char *);
typedef short(WINAPI *detailed_version_fn)(short, char *, char *, char *);
typedef short(WINAPI *error_msg_fn)(short, char *);
typedef short(WINAPI *last_error_msg_fn)(short, int *, char *, short);
typedef short(WINAPI *hardware_status_fn)(short, char *, short, short);
typedef short(WINAPI *send_command_fn)(short, short, const char *, short);
typedef short(WINAPI *send_message_fn)(short, unsigned char *, short, short, short);
typedef short(WINAPI *read_message_fn)(short, unsigned char *, short, short);

constexpr short cmd_set_all_filters_to_pass = 3;
constexpr short cmd_protect_j1939_address = 19;
constexpr short cmd_release_j1939_address = 31;
constexpr short cmd_set_blocktimeout = 215;

/* Offboard diagnostic tool 1: the address a service tool is expected to take. */
constexpr unsigned char tool_address = 0xF9;

/* An arbitrary NAME for the tool. Industry group 0, vehicle system 0, function 129 (offboard
 * diagnostic service tool), and a manufacturer code and identity that are ours to pick. */
const unsigned char tool_name[8] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x81, 0x00, 0x00};

/* Resolves an export and complains by name if it is missing, which is the symptom of a DLL built
 * without the .def file: the entry points end up decorated and nothing can find them. */
template <typename T>
T resolve(HMODULE dll, const char *name)
{
    T fn = reinterpret_cast<T>(GetProcAddress(dll, name));

    if (fn == nullptr)
    {
        printf("  FAIL %s not exported (error %lu)\n", name, GetLastError());
    }

    return fn;
}

bool starts_with_j1939(const char *protocol)
{
    return _strnicmp(protocol, "J1939", 5) == 0
           && (protocol[5] == '\0' || protocol[5] == ':' || protocol[5] == ',');
}

/* Claims an address, asks the bus who is out there, and prints what comes back.
 *
 * A CAN connection proves the driver reaches the device. This proves the far more interesting
 * thing: that the J1939 side works end to end and that there is something on the bus to talk to. */
void j1939_listen(HMODULE dll, short client, error_msg_fn get_error_msg)
{
    auto send_command = resolve<send_command_fn>(dll, "RP1210_SendCommand");
    auto send_message = resolve<send_message_fn>(dll, "RP1210_SendMessage");
    auto read_message = resolve<read_message_fn>(dll, "RP1210_ReadMessage");

    if (send_command == nullptr || send_message == nullptr || read_message == nullptr)
    {
        return;
    }

    char claim[10];

    claim[0] = (char)tool_address;
    memcpy(claim + 1, tool_name, 8);
    claim[9] = 0; /* Block until the contention window has closed. */

    printf("claiming address 0x%02X ... ", tool_address);

    const short claimed = send_command(cmd_protect_j1939_address, client, claim, sizeof(claim));

    if (claimed != 0)
    {
        char text[80] = {};

        get_error_msg(claimed, text);
        printf("FAILED\n  error %d: %s\n", claimed, text);

        if (claimed == 146 || claimed == 153)
        {
            printf("\nAnother node on the bus is already using 0x%02X and outranks us. That is a\n",
                tool_address);
            printf("working bus, not a broken driver: something else is claiming the same address.\n");
        }

        return;
    }

    printf("ok\n");

    const char pass = 0;

    send_command(cmd_set_all_filters_to_pass, client, &pass, 0);

    const char timeout[2] = {0, 100};

    send_command(cmd_set_blocktimeout, client, timeout, sizeof(timeout));

    /* Request the Address Claimed parameter group from everybody, so quiet nodes announce
     * themselves rather than leaving us waiting for periodic traffic. */
    unsigned char request[9];

    request[0] = 0x00; /* PGN 0x00EA00, little endian */
    request[1] = 0xEA;
    request[2] = 0x00;
    request[3] = 6;    /* priority */
    request[4] = tool_address;
    request[5] = 0xFF; /* to everybody */
    request[6] = 0x00; /* the parameter group being asked for: 0x00EE00 */
    request[7] = 0xEE;
    request[8] = 0x00;

    send_message(client, request, sizeof(request), 0, 0);

    printf("\nlistening for five seconds ...\n\n");

    const DWORD until = GetTickCount() + 5000;
    int count = 0;

    while (GetTickCount() < until)
    {
        unsigned char buffer[2048];
        const short size = read_message(client, buffer, sizeof(buffer), 0);

        if (size <= 10)
        {
            Sleep(5);

            continue;
        }

        const unsigned long pgn =
            (unsigned long)buffer[4] | ((unsigned long)buffer[5] << 8) | ((unsigned long)buffer[6] << 16);
        const unsigned char how = buffer[7];
        const unsigned char sa = buffer[8];
        const unsigned char da = buffer[9];
        const int length = size - 10;

        printf("  PGN 0x%05lX  pri %d  SA 0x%02X  DA 0x%02X  %3d bytes%s",
            pgn, how & 0x07, sa, da, length, (length > 8) ? "  [multi-packet]" : "");

        for (int i = 0; i < length && i < 8; i++)
        {
            printf(" %02X", buffer[10 + i]);
        }

        printf("%s\n", (length > 8) ? " ..." : "");

        count++;
    }

    printf("\n%d message(s) seen.\n", count);

    if (count == 0)
    {
        printf("\nNothing answered. The driver and the device are fine -- there is simply no J1939\n");
        printf("traffic on this bus. Check the wiring, the bit rate, and that the ECU is powered.\n");
    }

    const char release = (char)tool_address;

    send_command(cmd_release_j1939_address, client, &release, 1);
}

} // namespace

int main(int argc, char **argv)
{
    const short device_id = (short)((argc > 1) ? atoi(argv[1]) : 1);
    const char *protocol = (argc > 2) ? argv[2] : "CAN:Baud=250";

    printf("CANsub RP1210 connection test\n\n");

    printf("loading CANSUB32.dll ... ");

    HMODULE dll = LoadLibraryA("CANSUB32.dll");

    if (dll == nullptr)
    {
        const DWORD err = GetLastError();

        printf("FAILED (error %lu)\n\n", err);
        printf("The DLL was not found or could not be loaded. Check that:\n");
        printf("  - CANSUB32.dll is in %%WINDIR%%\\SysWOW64 (32-bit) or %%WINDIR%%\\System32 (64-bit),\n");
        printf("    or simply sitting next to this exe\n");
        printf("  - its bitness matches this exe: a 32-bit exe can only load a 32-bit DLL\n");

        return 1;
    }

    char path[MAX_PATH] = {};

    GetModuleFileNameA(dll, path, sizeof(path));
    printf("ok\n  %s\n\n", path);

    printf("resolving entry points ... ");

    auto client_connect = resolve<connect_fn>(dll, "RP1210_ClientConnect");
    auto client_disconnect = resolve<disconnect_fn>(dll, "RP1210_ClientDisconnect");
    auto read_version = resolve<read_version_fn>(dll, "RP1210_ReadVersion");
    auto detailed_version = resolve<detailed_version_fn>(dll, "RP1210_ReadDetailedVersion");
    auto get_error_msg = resolve<error_msg_fn>(dll, "RP1210_GetErrorMsg");
    auto hardware_status = resolve<hardware_status_fn>(dll, "RP1210_GetHardwareStatus");

    if (client_connect == nullptr || client_disconnect == nullptr || get_error_msg == nullptr)
    {
        return 1;
    }

    printf("ok\n\n");

    if (read_version != nullptr)
    {
        char dll_major[8] = {};
        char dll_minor[8] = {};
        char api_major[8] = {};
        char api_minor[8] = {};

        read_version(dll_major, dll_minor, api_major, api_minor);
        printf("driver version %s.%s, RP1210 API version %s.%s\n\n",
            dll_major, dll_minor, api_major, api_minor);
    }

    printf("connecting to DeviceID %d as \"%s\" ... ", device_id, protocol);

    const short client = client_connect(nullptr, device_id, protocol, 0, 0, 0);

    if (client < 0 || client > 127)
    {
        char text[80] = {};

        get_error_msg(client, text);
        printf("FAILED\n  error %d: %s\n", client, text);

        /* The driver usually knows more than the mnemonic says. Ask it. */
        auto last_error_msg = resolve<last_error_msg_fn>(dll, "RP1210_GetLastErrorMsg");

        if (last_error_msg != nullptr)
        {
            char detail[80] = {};
            int sub = 0;

            if (last_error_msg(client, &sub, detail, client) == 0 && detail[0] != '\0'
                && strcmp(detail, text) != 0)
            {
                printf("  %s\n", detail);
            }
        }

        printf("\n");

        switch (client)
        {
            case 134: /* ERR_INVALID_DEVICE */
            {
                printf("DeviceID %d is not in CANSUB32.ini, or its DeviceParams line is missing\n",
                    device_id);
                printf("a host= or channel= value. Check %%WINDIR%%\\CANSUB32.ini.\n");
                break;
            }
            case 135: /* ERR_DEVICE_IN_USE */
            {
                printf("The CAN channel already has a client. The device allows one at a time, so\n");
                printf("close any other tool using it (webCAN, a python-can script, another app).\n");
                break;
            }
            case 207: /* ERR_DEVICE_NOT_SUPPORTED */
            {
                printf("The device answered, but its REST API version is not the one this driver\n");
                printf("was built against. That is a firmware mismatch, not a wiring problem:\n");
                printf("the line above says which version it reported.\n\n");
                printf("  API 05.00 needs CANsub firmware 02.05.00 or later\n");
                printf("  API 04.00 is firmware 02.04.00, API 03.00 is firmware 02.03.00\n\n");
                printf("Update the device firmware, then try again. The current firmware and the\n");
                printf("update instructions are at:\n");
                printf("  https://canlogger.csselectronics.com/cansub-docs/\n");
                break;
            }
            case 136: /* ERR_INVALID_PROTOCOL */
            {
                printf("This driver serves \"CAN\" and \"J1939\". The name is matched whole, so\n");
                printf("\"CANopen\" and the like are refused; check the spelling, and check that\n");
                printf("this build is new enough to know the protocol you asked for.\n");
                break;
            }
            case 142: /* ERR_HARDWARE_NOT_RESPONDING */
            {
                printf("The device did not answer. Check that:\n");
                printf("  - it is powered and connected\n");
                printf("  - the host= name in CANSUB32.ini matches the device id on the label\n");
                printf("  - that name resolves: try \"ping <device-id>-usb.local\"\n");
                printf("  - if the name does not resolve, put the IP address in host= instead\n");
                break;
            }
            case 603: /* ERR_CANNOT_DETERMINE_BAUD_RATE */
            {
                printf("Baud=Auto listens for traffic and takes the rate that produces some, so a\n");
                printf("silent bus cannot be detected. Give the rate instead, for example\n");
                printf("\"J1939:Baud=250\".\n");
                break;
            }
            default:
            {
                break;
            }
        }

        return 1;
    }

    printf("ok, client id %d\n\n", client);

    if (detailed_version != nullptr)
    {
        char api[18] = {};
        char driver[18] = {};
        char firmware[18] = {};

        if (detailed_version(client, api, driver, firmware) == 0)
        {
            printf("device reports firmware %s (api %s, driver %s)\n", firmware, api, driver);
        }
    }

    if (hardware_status != nullptr)
    {
        char info[18] = {};

        if (hardware_status(client, info, sizeof(info), 0) == 0)
        {
            printf("hardware present: %s\n", info[0] ? "yes" : "no");
        }
    }

    if (starts_with_j1939(protocol))
    {
        printf("\n");
        j1939_listen(dll, client, get_error_msg);
    }

    printf("\ndisconnecting ... %s\n", (client_disconnect(client) == 0) ? "ok" : "failed");
    printf("\nConnection test passed.\n");

    FreeLibrary(dll);

    return 0;
}
