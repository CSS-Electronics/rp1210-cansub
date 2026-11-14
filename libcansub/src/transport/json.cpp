#include "json.hpp"

#include <cstdlib>

namespace cansub::json
{

namespace
{

/* Finds the position just past "key": , skipping whitespace. */
size_t find_value(const std::string &text, const std::string &key)
{
    const std::string needle = "\"" + key + "\"";
    size_t pos = text.find(needle);

    if (pos == std::string::npos)
    {
        return std::string::npos;
    }

    pos = text.find(':', pos + needle.size());

    if (pos == std::string::npos)
    {
        return std::string::npos;
    }

    pos++;

    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t'))
    {
        pos++;
    }

    return (pos < text.size()) ? pos : std::string::npos;
}

} // namespace

bool find_string(const std::string &text, const std::string &key, std::string &out)
{
    const size_t pos = find_value(text, key);

    if (pos == std::string::npos || text[pos] != '"')
    {
        return false;
    }

    const size_t end = text.find('"', pos + 1);

    if (end == std::string::npos)
    {
        return false;
    }

    out = text.substr(pos + 1, end - pos - 1);

    return true;
}

bool find_int(const std::string &text, const std::string &key, int64_t &out)
{
    const size_t pos = find_value(text, key);

    if (pos == std::string::npos)
    {
        return false;
    }

    if (text[pos] != '-' && (text[pos] < '0' || text[pos] > '9'))
    {
        return false;
    }

    out = strtoll(text.c_str() + pos, nullptr, 10);

    return true;
}

bool find_bool(const std::string &text, const std::string &key, bool &out)
{
    const size_t pos = find_value(text, key);

    if (pos == std::string::npos)
    {
        return false;
    }

    if (text.compare(pos, 4, "true") == 0)
    {
        out = true;

        return true;
    }

    if (text.compare(pos, 5, "false") == 0)
    {
        out = false;

        return true;
    }

    return false;
}

bool find_object(const std::string &text, const std::string &key, std::string &out)
{
    const size_t pos = find_value(text, key);

    if (pos == std::string::npos || text[pos] != '{')
    {
        return false;
    }

    int depth = 0;

    for (size_t i = pos; i < text.size(); i++)
    {
        depth += (text[i] == '{') ? 1 : 0;
        depth -= (text[i] == '}') ? 1 : 0;

        if (depth == 0)
        {
            out = text.substr(pos, i - pos + 1);

            return true;
        }
    }

    return false;
}

bool parse_int_array(const std::string &text, std::vector<int> &out)
{
    const size_t open = text.find('[');
    const size_t close = text.rfind(']');

    if (open == std::string::npos || close == std::string::npos || close < open)
    {
        return false;
    }

    out.clear();

    size_t pos = open + 1;

    while (pos < close)
    {
        while (pos < close && (text[pos] == ' ' || text[pos] == ',' || text[pos] == '\n'))
        {
            pos++;
        }

        if (pos >= close)
        {
            break;
        }

        if (text[pos] < '0' || text[pos] > '9')
        {
            return false;
        }

        out.push_back((int)strtol(text.c_str() + pos, nullptr, 10));

        while (pos < close && text[pos] >= '0' && text[pos] <= '9')
        {
            pos++;
        }
    }

    return true;
}

bool parse_string(const std::string &text, std::string &out)
{
    const size_t open = text.find('"');
    const size_t close = text.rfind('"');

    if (open == std::string::npos || close == open)
    {
        return false;
    }

    out = text.substr(open + 1, close - open - 1);

    return true;
}

void builder::separate()
{
    if (!out_.empty())
    {
        const char last = out_.back();

        if (last != '{' && last != ':' && last != '[')
        {
            out_ += ',';
        }
    }
}

builder &builder::key(const std::string &name)
{
    separate();
    out_ += "\"" + name + "\":";

    return *this;
}

builder &builder::value(const std::string &text)
{
    separate();
    out_ += "\"" + text + "\"";

    return *this;
}

builder &builder::value(int64_t number)
{
    separate();
    out_ += std::to_string(number);

    return *this;
}

builder &builder::value(bool flag)
{
    separate();
    out_ += flag ? "true" : "false";

    return *this;
}

builder &builder::object_begin()
{
    separate();
    out_ += '{';

    return *this;
}

builder &builder::object_end()
{
    out_ += '}';

    return *this;
}

builder &builder::field(const std::string &name, const std::string &text)
{
    return key(name).value(text);
}

builder &builder::field(const std::string &name, int64_t number)
{
    return key(name).value(number);
}

builder &builder::field(const std::string &name, bool flag)
{
    return key(name).value(flag);
}

std::string builder::str() const
{
    return out_;
}

} // namespace cansub::json
