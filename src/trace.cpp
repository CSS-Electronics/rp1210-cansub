#include "trace.hpp"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "ini.hpp"

namespace rp1210
{
namespace trace
{

namespace
{

std::mutex &mutex()
{
    static std::mutex m;

    return m;
}

struct config_t
{
    int level = 0;
    std::string path;
    FILE *file = nullptr;
};

config_t &config()
{
    static config_t c;

    return c;
}

std::atomic<int> cached_level{-1}; /* -1 until the INI has been read. */

/* Reads the settings once, on the first call through any entry point.
 *
 * Deliberately not done from DllMain: opening a file there is one of the things the loader lock
 * makes unsafe, and getting that wrong would hang the host application rather than trace it. */
void initialise()
{
    config_t &c = config();

    std::string path = vendor_key("DebugFile");
    std::string level_text = vendor_key("DebugLevel");

    /* An environment variable is the way in when the INI lives in the Windows directory and editing
     * it would need administrator rights. */
    char env_path[MAX_PATH] = {};

    if (GetEnvironmentVariableA("CANSUB_RP1210_LOG", env_path, sizeof(env_path)) > 0)
    {
        path = env_path;

        if (level_text.empty())
        {
            level_text = "2";
        }
    }

    char env_level[16] = {};

    if (GetEnvironmentVariableA("CANSUB_RP1210_LOG_LEVEL", env_level, sizeof(env_level)) > 0)
    {
        level_text = env_level;
    }

    const int wanted = level_text.empty() ? 0 : atoi(level_text.c_str());

    if (wanted <= 0 || path.empty())
    {
        cached_level.store(0, std::memory_order_relaxed);

        return;
    }

    c.file = nullptr;
    fopen_s(&c.file, path.c_str(), "a");

    if (c.file == nullptr)
    {
        /* Nowhere to write is not worth failing a connection over. */
        cached_level.store(0, std::memory_order_relaxed);

        return;
    }

    c.level = wanted;
    c.path = path;

    cached_level.store(wanted, std::memory_order_relaxed);

    SYSTEMTIME now;

    GetLocalTime(&now);
    fprintf(c.file,
            "\n=== CANSUB32 trace opened %04d-%02d-%02d %02d:%02d:%02d  level %d  pid %lu ===\n",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, wanted,
            GetCurrentProcessId());
    fprintf(c.file, "ini: %s\n", ini_path().c_str());
    fflush(c.file);
}

void ensure_initialised()
{
    if (cached_level.load(std::memory_order_relaxed) >= 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex());

    if (cached_level.load(std::memory_order_relaxed) < 0)
    {
        initialise();
    }
}

} // namespace

bool enabled()
{
    ensure_initialised();

    return cached_level.load(std::memory_order_relaxed) > 0;
}

int level()
{
    ensure_initialised();

    return cached_level.load(std::memory_order_relaxed);
}

const std::string &path()
{
    ensure_initialised();

    return config().path;
}

void printf_line(const char *format, ...)
{
    std::lock_guard<std::mutex> lock(mutex());

    config_t &c = config();

    if (c.file == nullptr)
    {
        return;
    }

    /* Milliseconds since the process started: the gaps between calls are what matter when an
     * application is deciding something has timed out. */
    fprintf(c.file, "[%8lu] ", GetTickCount());

    va_list args;

    va_start(args, format);
    vfprintf(c.file, format, args);
    va_end(args);

    fputc('\n', c.file);

    /* Flushed every line, so an application that crashes or is killed still leaves the trace that
     * explains why. */
    fflush(c.file);
}

std::string hex(const void *data, size_t size, size_t limit)
{
    if (data == nullptr)
    {
        return "(null)";
    }

    const unsigned char *bytes = (const unsigned char *)data;
    const size_t shown = (size > limit) ? limit : size;

    std::string out;

    out.reserve(shown * 3 + 8);

    for (size_t i = 0; i < shown; i++)
    {
        char pair[4];

        snprintf(pair, sizeof(pair), "%02X ", bytes[i]);
        out += pair;
    }

    if (size > shown)
    {
        char more[32];

        snprintf(more, sizeof(more), "... (%zu bytes)", size);
        out += more;
    }

    return out;
}

} // namespace trace
} // namespace rp1210
