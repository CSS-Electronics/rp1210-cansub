#include "socket.hpp"

#include <mutex>

#pragma comment(lib, "ws2_32.lib")

namespace cansub::sdk
{

void ensure_winsock()
{
    static std::once_flag once;

    std::call_once(once, [] {
        WSADATA data;

        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

bool resolve_ipv4(const std::string &host, uint16_t port, sockaddr_in &out, std::string &error)
{
    ensure_winsock();

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo *result = nullptr;
    const int rc = getaddrinfo(host.c_str(), nullptr, &hints, &result);

    if (rc != 0 || result == nullptr)
    {
        error = "cannot resolve " + host + " to an IPv4 address (error " + std::to_string(rc) + ")";

        return false;
    }

    out = *(const sockaddr_in *)result->ai_addr;
    out.sin_port = htons(port);
    freeaddrinfo(result);

    return true;
}

tcp_socket::~tcp_socket()
{
    close();
}

bool tcp_socket::start_connect(const sockaddr_in &address, int &wsa_error)
{
    close();
    ensure_winsock();

    handle_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (handle_ == INVALID_SOCKET)
    {
        wsa_error = WSAGetLastError();

        return false;
    }

    u_long non_blocking = 1;
    ioctlsocket(handle_, FIONBIO, &non_blocking);

    /* CAN frames are small and latency matters more than packing them. */
    BOOL no_delay = TRUE;
    setsockopt(handle_, IPPROTO_TCP, TCP_NODELAY, (const char *)&no_delay, sizeof(no_delay));

    /* Notices a device that vanished (cable pulled) on an otherwise idle connection. */
    BOOL keep_alive = TRUE;
    setsockopt(handle_, SOL_SOCKET, SO_KEEPALIVE, (const char *)&keep_alive, sizeof(keep_alive));

    if (connect(handle_, (const sockaddr *)&address, sizeof(address)) == SOCKET_ERROR)
    {
        wsa_error = WSAGetLastError();

        if (wsa_error != WSAEWOULDBLOCK)
        {
            close();

            return false;
        }
    }

    wsa_error = 0;

    return true;
}

bool tcp_socket::finish_connect(int &wsa_error)
{
    int error = 0;
    int size = sizeof(error);

    if (getsockopt(handle_, SOL_SOCKET, SO_ERROR, (char *)&error, &size) == SOCKET_ERROR)
    {
        wsa_error = WSAGetLastError();

        return false;
    }

    wsa_error = error;

    return error == 0;
}

int tcp_socket::send_some(const void *data, size_t size)
{
    const int chunk = (size > 1u << 20) ? (1 << 20) : (int)size;
    const int sent = ::send(handle_, (const char *)data, chunk, 0);

    if (sent == SOCKET_ERROR)
    {
        return (WSAGetLastError() == WSAEWOULDBLOCK) ? 0 : -1;
    }

    return sent;
}

int tcp_socket::recv_some(void *data, size_t size)
{
    const int chunk = (size > 1u << 20) ? (1 << 20) : (int)size;
    const int got = ::recv(handle_, (char *)data, chunk, 0);

    if (got == SOCKET_ERROR)
    {
        return (WSAGetLastError() == WSAEWOULDBLOCK) ? -1 : -2;
    }

    return got;
}

void tcp_socket::close()
{
    if (handle_ != INVALID_SOCKET)
    {
        closesocket(handle_);
        handle_ = INVALID_SOCKET;
    }
}

} // namespace cansub::sdk
