#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cctv
{
namespace json
{

// Small, dependency-free JSON DOM. The EPG response is small (a few hundred
// entries) and only needs objects, arrays, strings and integers, but the
// parser still supports the complete JSON grammar.
class Value
{
public:
  enum class Type
  {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
  };

  Value() = default;
  explicit Value(bool value) : m_type(Type::Bool), m_bool(value) {}
  explicit Value(double value) : m_type(Type::Number), m_number(value) {}
  explicit Value(std::string value) : m_type(Type::String), m_string(std::move(value)) {}
  explicit Value(std::vector<Value> value) : m_type(Type::Array), m_array(std::move(value)) {}
  explicit Value(std::map<std::string, Value> value)
    : m_type(Type::Object), m_object(std::move(value))
  {
  }

  Type GetType() const { return m_type; }
  bool IsNull() const { return m_type == Type::Null; }
  bool IsBool() const { return m_type == Type::Bool; }
  bool IsNumber() const { return m_type == Type::Number; }
  bool IsString() const { return m_type == Type::String; }
  bool IsArray() const { return m_type == Type::Array; }
  bool IsObject() const { return m_type == Type::Object; }

  bool GetBool() const { return m_type == Type::Bool ? m_bool : false; }
  double GetNumber() const { return m_type == Type::Number ? m_number : 0.0; }
  int64_t GetInt64() const { return static_cast<int64_t>(GetNumber()); }
  const std::string& GetString() const { return m_string; }
  const std::vector<Value>& GetArray() const { return m_array; }
  const std::map<std::string, Value>& GetObject() const { return m_object; }

  const Value* Find(const std::string& key) const
  {
    if (m_type != Type::Object)
      return nullptr;
    const auto it = m_object.find(key);
    return it == m_object.end() ? nullptr : &it->second;
  }

private:
  Type m_type = Type::Null;
  bool m_bool = false;
  double m_number = 0.0;
  std::string m_string;
  std::vector<Value> m_array;
  std::map<std::string, Value> m_object;
};

bool Parse(const std::string& text, Value& out, std::string& error);

} // namespace json
} // namespace cctv
