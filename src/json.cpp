#include "json.h"

#include <cstdlib>
#include <utility>

namespace cctv
{
namespace json
{
namespace
{

constexpr int kMaxDepth = 64;

class Parser
{
public:
  explicit Parser(const std::string& text) : m_text(text) {}

  bool ParseDocument(Value& out)
  {
    SkipWhitespace();
    if (!ParseValue(out, 0))
      return false;
    SkipWhitespace();
    if (m_pos != m_text.size())
      return Fail("trailing characters");
    return true;
  }

  const std::string& GetError() const { return m_error; }

private:
  bool Fail(const std::string& message)
  {
    m_error = message + " at offset " + std::to_string(m_pos);
    return false;
  }

  void SkipWhitespace()
  {
    while (m_pos < m_text.size())
    {
      const char c = m_text[m_pos];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
        break;
      ++m_pos;
    }
  }

  bool ParseValue(Value& out, int depth)
  {
    if (depth > kMaxDepth)
      return Fail("nesting too deep");
    if (m_pos >= m_text.size())
      return Fail("unexpected end of input");

    const char c = m_text[m_pos];
    switch (c)
    {
      case '{':
        return ParseObject(out, depth);
      case '[':
        return ParseArray(out, depth);
      case '"':
      {
        std::string value;
        if (!ParseString(value))
          return false;
        out = Value(std::move(value));
        return true;
      }
      case 't':
        return ParseLiteral("true", Value(true), out);
      case 'f':
        return ParseLiteral("false", Value(false), out);
      case 'n':
        return ParseLiteral("null", Value(), out);
      default:
        if (c == '-' || (c >= '0' && c <= '9'))
          return ParseNumber(out);
        return Fail("unexpected character");
    }
  }

  bool ParseLiteral(const char* literal, Value value, Value& out)
  {
    const std::size_t length = std::char_traits<char>::length(literal);
    if (m_text.compare(m_pos, length, literal) != 0)
      return Fail("invalid literal");
    m_pos += length;
    out = std::move(value);
    return true;
  }

  bool ParseObject(Value& out, int depth)
  {
    ++m_pos; // '{'
    std::map<std::string, Value> object;
    SkipWhitespace();

    if (m_pos < m_text.size() && m_text[m_pos] == '}')
    {
      ++m_pos;
      out = Value(std::move(object));
      return true;
    }

    for (;;)
    {
      SkipWhitespace();
      if (m_pos >= m_text.size() || m_text[m_pos] != '"')
        return Fail("expected object key");

      std::string key;
      if (!ParseString(key))
        return false;

      SkipWhitespace();
      if (m_pos >= m_text.size() || m_text[m_pos] != ':')
        return Fail("expected ':'");
      ++m_pos;

      SkipWhitespace();
      Value value;
      if (!ParseValue(value, depth + 1))
        return false;
      object[std::move(key)] = std::move(value);

      SkipWhitespace();
      if (m_pos >= m_text.size())
        return Fail("unterminated object");
      if (m_text[m_pos] == '}')
      {
        ++m_pos;
        break;
      }
      if (m_text[m_pos] != ',')
        return Fail("expected ',' or '}'");
      ++m_pos;
    }

    out = Value(std::move(object));
    return true;
  }

  bool ParseArray(Value& out, int depth)
  {
    ++m_pos; // '['
    std::vector<Value> array;
    SkipWhitespace();

    if (m_pos < m_text.size() && m_text[m_pos] == ']')
    {
      ++m_pos;
      out = Value(std::move(array));
      return true;
    }

    for (;;)
    {
      SkipWhitespace();
      Value value;
      if (!ParseValue(value, depth + 1))
        return false;
      array.push_back(std::move(value));

      SkipWhitespace();
      if (m_pos >= m_text.size())
        return Fail("unterminated array");
      if (m_text[m_pos] == ']')
      {
        ++m_pos;
        break;
      }
      if (m_text[m_pos] != ',')
        return Fail("expected ',' or ']'");
      ++m_pos;
    }

    out = Value(std::move(array));
    return true;
  }

  bool ParseNumber(Value& out)
  {
    const std::size_t start = m_pos;

    if (m_text[m_pos] == '-')
      ++m_pos;

    if (m_pos >= m_text.size())
      return Fail("invalid number");

    if (m_text[m_pos] == '0')
    {
      ++m_pos;
    }
    else if (m_text[m_pos] >= '1' && m_text[m_pos] <= '9')
    {
      while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
        ++m_pos;
    }
    else
    {
      return Fail("invalid number");
    }

    if (m_pos < m_text.size() && m_text[m_pos] == '.')
    {
      ++m_pos;
      if (m_pos >= m_text.size() || m_text[m_pos] < '0' || m_text[m_pos] > '9')
        return Fail("invalid number fraction");
      while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
        ++m_pos;
    }

    if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E'))
    {
      ++m_pos;
      if (m_pos < m_text.size() && (m_text[m_pos] == '+' || m_text[m_pos] == '-'))
        ++m_pos;
      if (m_pos >= m_text.size() || m_text[m_pos] < '0' || m_text[m_pos] > '9')
        return Fail("invalid number exponent");
      while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
        ++m_pos;
    }

    const std::string number = m_text.substr(start, m_pos - start);
    char* end = nullptr;
    const double value = std::strtod(number.c_str(), &end);
    if (end == nullptr || end != number.c_str() + number.size())
      return Fail("invalid number");

    out = Value(value);
    return true;
  }

  bool ParseString(std::string& out)
  {
    if (m_pos >= m_text.size() || m_text[m_pos] != '"')
      return Fail("expected string");
    ++m_pos;

    out.clear();
    while (m_pos < m_text.size())
    {
      const char c = m_text[m_pos++];
      if (c == '"')
        return true;

      if (c == '\\')
      {
        if (m_pos >= m_text.size())
          return Fail("unterminated escape");
        const char escape = m_text[m_pos++];
        switch (escape)
        {
          case '"':
            out.push_back('"');
            break;
          case '\\':
            out.push_back('\\');
            break;
          case '/':
            out.push_back('/');
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u':
          {
            uint32_t codepoint = 0;
            if (!ParseHex4(codepoint))
              return false;

            if (codepoint >= 0xD800 && codepoint <= 0xDBFF)
            {
              // UTF-16 surrogate pair.
              if (m_pos + 6 <= m_text.size() && m_text[m_pos] == '\\' &&
                  m_text[m_pos + 1] == 'u')
              {
                const std::size_t saved = m_pos;
                m_pos += 2;
                uint32_t low = 0;
                if (!ParseHex4(low))
                  return false;
                if (low >= 0xDC00 && low <= 0xDFFF)
                  codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                else
                {
                  m_pos = saved;
                  codepoint = 0xFFFD;
                }
              }
              else
              {
                codepoint = 0xFFFD;
              }
            }
            else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF)
            {
              codepoint = 0xFFFD;
            }

            if (codepoint == 0)
              codepoint = 0xFFFD;
            AppendUtf8(out, codepoint);
            break;
          }
          default:
            return Fail("invalid escape");
        }
      }
      else if (static_cast<unsigned char>(c) < 0x20)
      {
        return Fail("control character in string");
      }
      else
      {
        out.push_back(c);
      }
    }

    return Fail("unterminated string");
  }

  bool ParseHex4(uint32_t& value)
  {
    if (m_pos + 4 > m_text.size())
      return Fail("incomplete unicode escape");

    value = 0;
    for (int i = 0; i < 4; ++i)
    {
      const char c = m_text[m_pos++];
      int digit = -1;
      if (c >= '0' && c <= '9')
        digit = c - '0';
      else if (c >= 'a' && c <= 'f')
        digit = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        digit = c - 'A' + 10;

      if (digit < 0)
        return Fail("invalid unicode escape");
      value = (value << 4) | static_cast<uint32_t>(digit);
    }
    return true;
  }

  static void AppendUtf8(std::string& out, uint32_t codepoint)
  {
    if (codepoint <= 0x7F)
    {
      out.push_back(static_cast<char>(codepoint));
    }
    else if (codepoint <= 0x7FF)
    {
      out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else if (codepoint <= 0xFFFF)
    {
      out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else if (codepoint <= 0x10FFFF)
    {
      out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else
    {
      out += "\xEF\xBF\xBD"; // U+FFFD
    }
  }

  const std::string& m_text;
  std::size_t m_pos = 0;
  std::string m_error;
};

} // namespace

bool Parse(const std::string& text, Value& out, std::string& error)
{
  Parser parser(text);
  if (!parser.ParseDocument(out))
  {
    error = parser.GetError();
    return false;
  }
  return true;
}

} // namespace json
} // namespace cctv
