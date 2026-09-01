#pragma once

#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <cstdint>
#include <iostream>

namespace study::formal {

enum class JsonType {
    kNull,
    kBool,
    kNumber,
    kString,
    kArray,
    kObject
};

class JsonValue {
public:
    JsonType type = JsonType::kNull;
    bool bool_val = false;
    double num_val = 0.0;
    std::string str_val;
    std::vector<JsonValue> arr_val;
    std::map<std::string, JsonValue> obj_val;

    bool IsNull() const { return type == JsonType::kNull; }
    bool IsBool() const { return type == JsonType::kBool; }
    bool IsNumber() const { return type == JsonType::kNumber; }
    bool IsString() const { return type == JsonType::kString; }
    bool IsArray() const { return type == JsonType::kArray; }
    bool IsObject() const { return type == JsonType::kObject; }

    std::string AsString(const std::string& default_val = "") const {
        return IsString() ? str_val : default_val;
    }

    int64_t AsInt64(int64_t default_val = 0) const {
        return IsNumber() ? static_cast<int64_t>(num_val) : default_val;
    }

    uint64_t AsUInt64(uint64_t default_val = 0) const {
        return IsNumber() ? static_cast<uint64_t>(num_val) : default_val;
    }

    double AsDouble(double default_val = 0.0) const {
        return IsNumber() ? num_val : default_val;
    }

    bool AsBool(bool default_val = false) const {
        return IsBool() ? bool_val : default_val;
    }

    bool Has(const std::string& key) const {
        return IsObject() && obj_val.find(key) != obj_val.end();
    }

    const JsonValue& operator[](const std::string& key) const {
        if (!IsObject()) {
            static const JsonValue kNullValue;
            return kNullValue;
        }
        auto it = obj_val.find(key);
        if (it != obj_val.end()) {
            return it->second;
        }
        static const JsonValue kNullValue;
        return kNullValue;
    }

    const JsonValue& operator[](size_t index) const {
        if (!IsArray() || index >= arr_val.size()) {
            static const JsonValue kNullValue;
            return kNullValue;
        }
        return arr_val[index];
    }
};

class JsonParser {
public:
    static bool Parse(const std::string& json_str, JsonValue& out_root, std::string& out_error) {
        try {
            size_t pos = 0;
            SkipWhitespace(json_str, pos);
            if (pos >= json_str.size()) {
                out_error = "Empty JSON input";
                return false;
            }
            out_root = ParseValue(json_str, pos);
            SkipWhitespace(json_str, pos);
            if (pos < json_str.size()) {
                out_error = "Trailing characters after JSON root at offset " + std::to_string(pos);
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            out_error = e.what();
            return false;
        }
    }

private:
    static void SkipWhitespace(const std::string& s, size_t& pos) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) {
            pos++;
        }
    }

    static JsonValue ParseValue(const std::string& s, size_t& pos) {
        SkipWhitespace(s, pos);
        if (pos >= s.size()) throw std::runtime_error("Unexpected end of JSON input");

        char c = s[pos];
        if (c == '{') return ParseObject(s, pos);
        if (c == '[') return ParseArray(s, pos);
        if (c == '"') return ParseString(s, pos);
        if (c == 't' || c == 'f') return ParseBool(s, pos);
        if (c == 'n') return ParseNull(s, pos);
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(s, pos);

        throw std::runtime_error(std::string("Unexpected token '") + c + "' at position " + std::to_string(pos));
    }

    static JsonValue ParseObject(const std::string& s, size_t& pos) {
        pos++; // Skip '{'
        JsonValue val;
        val.type = JsonType::kObject;

        while (true) {
            SkipWhitespace(s, pos);
            if (pos >= s.size()) throw std::runtime_error("Unterminated object");
            if (s[pos] == '}') {
                pos++;
                break;
            }

            if (s[pos] != '"') throw std::runtime_error("Expected string key in object at pos " + std::to_string(pos));
            JsonValue key_val = ParseString(s, pos);
            std::string key = key_val.str_val;

            SkipWhitespace(s, pos);
            if (pos >= s.size() || s[pos] != ':') throw std::runtime_error("Expected ':' after key at pos " + std::to_string(pos));
            pos++; // Skip ':'

            JsonValue child = ParseValue(s, pos);
            val.obj_val[key] = child;

            SkipWhitespace(s, pos);
            if (pos >= s.size()) throw std::runtime_error("Unterminated object");
            if (s[pos] == ',') {
                pos++;
            } else if (s[pos] == '}') {
                pos++;
                break;
            } else {
                throw std::runtime_error("Expected ',' or '}' in object at pos " + std::to_string(pos));
            }
        }
        return val;
    }

    static JsonValue ParseArray(const std::string& s, size_t& pos) {
        pos++; // Skip '['
        JsonValue val;
        val.type = JsonType::kArray;

        while (true) {
            SkipWhitespace(s, pos);
            if (pos >= s.size()) throw std::runtime_error("Unterminated array");
            if (s[pos] == ']') {
                pos++;
                break;
            }

            val.arr_val.push_back(ParseValue(s, pos));

            SkipWhitespace(s, pos);
            if (pos >= s.size()) throw std::runtime_error("Unterminated array");
            if (s[pos] == ',') {
                pos++;
            } else if (s[pos] == ']') {
                pos++;
                break;
            } else {
                throw std::runtime_error("Expected ',' or ']' in array at pos " + std::to_string(pos));
            }
        }
        return val;
    }

    static JsonValue ParseString(const std::string& s, size_t& pos) {
        pos++; // Skip '"'
        std::string res;
        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '"') {
                JsonValue val;
                val.type = JsonType::kString;
                val.str_val = res;
                return val;
            }
            if (c == '\\') {
                if (pos >= s.size()) throw std::runtime_error("Unterminated escape sequence");
                char esc = s[pos++];
                if (esc == '"') res += '"';
                else if (esc == '\\') res += '\\';
                else if (esc == '/') res += '/';
                else if (esc == 'b') res += '\b';
                else if (esc == 'f') res += '\f';
                else if (esc == 'n') res += '\n';
                else if (esc == 'r') res += '\r';
                else if (esc == 't') res += '\t';
                else res += esc;
            } else {
                res += c;
            }
        }
        throw std::runtime_error("Unterminated string literal");
    }

    static JsonValue ParseNumber(const std::string& s, size_t& pos) {
        size_t start = pos;
        if (s[pos] == '-') pos++;
        while (pos < s.size() && (s[pos] >= '0' && s[pos] <= '9')) pos++;
        if (pos < s.size() && s[pos] == '.') {
            pos++;
            while (pos < s.size() && (s[pos] >= '0' && s[pos] <= '9')) pos++;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            pos++;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos++;
            while (pos < s.size() && (s[pos] >= '0' && s[pos] <= '9')) pos++;
        }

        std::string num_str = s.substr(start, pos - start);
        JsonValue val;
        val.type = JsonType::kNumber;
        val.num_val = std::stod(num_str);
        return val;
    }

    static JsonValue ParseBool(const std::string& s, size_t& pos) {
        if (s.compare(pos, 4, "true") == 0) {
            pos += 4;
            JsonValue val;
            val.type = JsonType::kBool;
            val.bool_val = true;
            return val;
        } else if (s.compare(pos, 5, "false") == 0) {
            pos += 5;
            JsonValue val;
            val.type = JsonType::kBool;
            val.bool_val = false;
            return val;
        }
        throw std::runtime_error("Invalid boolean at pos " + std::to_string(pos));
    }

    static JsonValue ParseNull(const std::string& s, size_t& pos) {
        if (s.compare(pos, 4, "null") == 0) {
            pos += 4;
            JsonValue val;
            val.type = JsonType::kNull;
            return val;
        }
        throw std::runtime_error("Invalid null at pos " + std::to_string(pos));
    }
};

} // namespace study::formal
