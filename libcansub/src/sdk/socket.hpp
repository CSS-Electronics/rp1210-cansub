#ifndef CANSUB_SDK_SOCKET_HPP
#define CANSUB_SDK_SOCKET_HPP

/* A non-blocking IPv4 TCP socket for the SDK glue. The SDK owns no sockets: it hands out bytes to
 * write and takes bytes that were read, so this is all the network code the glue needs. */

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstddef>
#include <string>

namespace cansub::sdk
{

/* Starts Winsock once per process and never stops it: a DLL cannot know when its host is finished
 * with sockets of its own. */
void ensure_winsock();

/* Resolves host to an IPv4 address (the device is IPv4 only). ".local" names go through the
 * Windows resolver. */
bool resolve_ipv4(const std::string &host, uint16_t port, sockaddr_in &out, std::string &error);

class tcp_socket
{
public:
    tcp_socket() = default;
    ~tcp_socket();

    tcp_socket(const tcp_socket &) = delete;
    tcp_socket &operator=(const tcp_socket &) = delete;

    /* Starts a non-blocking connect. Completion shows as writable in poll(); then call
     * finish_connect(). */
    bool start_connect(const sockaddr_in &address, int &wsa_error);

    /* After poll() reports writable or an error on a connecting socket. */
    bool finish_connect(int &wsa_error);

    /* Bytes written, 0 if the socket would block, -1 on error. */
    int send_some(const void *data, size_t size);

    /* Bytes read, 0 at end of stream, -1 if it would block, -2 on error. */
    int recv_some(void *data, size_t size);

    void close();

    bool valid() const
    {
        return handle_ != INVALID_SOCKET;
    }

    SOCKET handle() const
    {
        return handle_;
    }

private:
    SOCKET handle_ = INVALID_SOCKET;
};

} // namespace cansub::sdk

#endif
