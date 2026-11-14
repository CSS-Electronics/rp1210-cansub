#include "mdns.hpp"

#include <cstdio>
#include <cstring>

namespace cansub
{
namespace mdns
{

namespace
{

/* A name is a chain of length-prefixed labels, optionally ending in a pointer back into the message
 * so a repeated suffix is not repeated on the wire. Two things have to be defended against: a
 * pointer that jumps forwards or to itself, which can loop for ever, and a length that runs off the
 * end of the buffer. */
bool read_name(const uint8_t *data, size_t size, size_t offset, std::string &out, size_t &after)
{
    constexpr int max_jumps = 32;
    constexpr size_t max_name = 255;

    int jumps = 0;
    bool jumped = false;

    out.clear();
    after = offset;

    for (;;)
    {
        if (offset >= size)
        {
            return false;
        }

        const uint8_t length = data[offset];

        if (length == 0)
        {
            offset++;

            if (!jumped)
            {
                after = offset;
            }

            return true;
        }

        if ((length & 0xC0) == 0xC0)
        {
            if (offset + 1 >= size)
            {
                return false;
            }

            const size_t target = (size_t)(((length & 0x3F) << 8) | data[offset + 1]);

            if (!jumped)
            {
                after = offset + 2;
            }

            /* Only ever backwards, and a bounded number of times. */
            if (target >= offset || ++jumps > max_jumps)
            {
                return false;
            }

            offset = target;
            jumped = true;

            continue;
        }

        if ((length & 0xC0) != 0)
        {
            return false; /* reserved label type */
        }

        if (offset + 1 + length > size || out.size() + length + 1 > max_name)
        {
            return false;
        }

        if (!out.empty())
        {
            out += '.';
        }

        out.append((const char *)&data[offset + 1], length);
        offset += 1 + length;
    }
}

void append_name(std::vector<uint8_t> &out, const std::string &name)
{
    size_t start = 0;

    while (start <= name.size())
    {
        const size_t dot = name.find('.', start);
        const size_t end = (dot == std::string::npos) ? name.size() : dot;
        const size_t length = end - start;

        if (length == 0)
        {
            break;
        }

        out.push_back((uint8_t)length);
        out.insert(out.end(), name.begin() + (ptrdiff_t)start, name.begin() + (ptrdiff_t)end);

        if (dot == std::string::npos)
        {
            break;
        }

        start = dot + 1;
    }

    out.push_back(0);
}

bool read_u16(const uint8_t *data, size_t size, size_t offset, uint16_t &out)
{
    if (offset + 2 > size)
    {
        return false;
    }

    out = (uint16_t)((data[offset] << 8) | data[offset + 1]);

    return true;
}

} // namespace

std::vector<uint8_t> query(const std::string &name, uint16_t type)
{
    std::vector<uint8_t> out;

    /* Transaction id 0, no flags: multicast DNS matches on the question, not on an id. One question,
     * no answers. */
    const uint8_t header[12] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};

    out.insert(out.end(), header, header + sizeof(header));
    append_name(out, name);

    out.push_back((uint8_t)(type >> 8));
    out.push_back((uint8_t)(type & 0xFF));
    out.push_back(0x00);
    out.push_back(0x01); /* class IN */

    return out;
}

bool parse(const uint8_t *data, size_t size, std::vector<record_t> &out)
{
    constexpr size_t max_records = 128;

    if (data == nullptr || size < 12)
    {
        return false;
    }

    uint16_t questions = 0;
    uint16_t answers = 0;
    uint16_t authority = 0;
    uint16_t additional = 0;

    if (!read_u16(data, size, 4, questions) || !read_u16(data, size, 6, answers)
        || !read_u16(data, size, 8, authority) || !read_u16(data, size, 10, additional))
    {
        return false;
    }

    size_t offset = 12;

    for (uint16_t i = 0; i < questions; i++)
    {
        std::string name;

        if (!read_name(data, size, offset, name, offset))
        {
            return false;
        }

        offset += 4; /* type and class */

        if (offset > size)
        {
            return false;
        }
    }

    const size_t total = (size_t)answers + authority + additional;

    for (size_t i = 0; i < total; i++)
    {
        record_t record;

        if (!read_name(data, size, offset, record.name, offset))
        {
            return false;
        }

        uint16_t rclass = 0;
        uint16_t length = 0;

        if (!read_u16(data, size, offset, record.type) || !read_u16(data, size, offset + 2, rclass)
            || !read_u16(data, size, offset + 8, length))
        {
            return false;
        }

        offset += 10; /* type, class, ttl, rdlength */

        if (offset + length > size)
        {
            return false;
        }

        const size_t rdata = offset;

        switch (record.type)
        {
            case type_a:
            {
                if (length == 4)
                {
                    char text[16];

                    snprintf(text, sizeof(text), "%u.%u.%u.%u", data[rdata], data[rdata + 1],
                             data[rdata + 2], data[rdata + 3]);
                    record.target = text;
                }

                break;
            }

            case type_ptr:
            {
                size_t ignored = 0;

                read_name(data, size, rdata, record.target, ignored);

                break;
            }

            case type_srv:
            {
                if (length >= 7 && read_u16(data, size, rdata + 4, record.port))
                {
                    size_t ignored = 0;

                    read_name(data, size, rdata + 6, record.target, ignored);
                }

                break;
            }

            case type_txt:
            {
                size_t at = rdata;

                while (at < rdata + length)
                {
                    const uint8_t item = data[at];

                    if (item == 0 || at + 1 + item > rdata + length)
                    {
                        break;
                    }

                    record.txt.emplace_back((const char *)&data[at + 1], item);
                    at += 1 + item;
                }

                break;
            }

            default:
            {
                break;
            }
        }

        offset += length;

        if (out.size() < max_records)
        {
            out.push_back(std::move(record));
        }
    }

    return true;
}

} // namespace mdns
} // namespace cansub
