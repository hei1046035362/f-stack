#include "Serialize.hpp"
#include <string>
#include <stdexcept>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class PhpSerializer {
public:
    // Unserialize PHP serialized string to JSON
    static json unserialize(const std::string& input) {
        size_t pos = 0;
        return parseValue(input, pos);
    }

    // Serialize JSON to PHP serialized string
    static std::string serialize(const json& j) {
        std::string result;
        // Estimate initial capacity to reduce reallocations
        size_t estimated_size = estimateSerializedSize(j);
        result.reserve(estimated_size);
        serializeValue(j, result);
        return result;
    }

    // PHP's array_replace_recursive equivalent
    static json array_replace_recursive(const json& base, const json& replacement) {
        if (!base.is_object() && !base.is_array()) {
            return replacement;
        }
        if (!replacement.is_object() && !replacement.is_array()) {
            return replacement;
        }

        json result = base;
        for (auto& item : replacement.items()) {
            if (base.contains(item.key())) {
                if (base[item.key()].is_object() && item.value().is_object()) {
                    result[item.key()] = array_replace_recursive(base[item.key()], item.value());
                } else {
                    result[item.key()] = item.value();
                }
            } else {
                result[item.key()] = item.value();
            }
        }
        return result;
    }

private:
    // Parse a single value from PHP serialized string
    static json parseValue(const std::string& input, size_t& pos) {
        if (pos >= input.length()) {
            throw std::runtime_error("Unexpected end of input");
        }

        char type = input[pos];
        pos += 2; // Skip type and colon

        switch (type) {
            case 's': return parseString(input, pos);
            case 'i': return parseInteger(input, pos);
            case 'd': return parseDouble(input, pos);
            case 'b': return parseBoolean(input, pos);
            case 'a': return parseArray(input, pos);
            case 'N': pos++; return nullptr;
            default: throw std::runtime_error("Unknown type: " + std::string(1, type));
        }
    }

    static std::string parseString(const std::string& input, size_t& pos) {
        size_t colon = input.find(':', pos);
        if (colon == std::string::npos) {
            throw std::runtime_error("Invalid string format");
        }
        int length = std::stoi(input.substr(pos, colon - pos));
        pos = colon + 2;
        std::string result = input.substr(pos, length);
        pos += length + 2;
        return result;
    }

    static int parseInteger(const std::string& input, size_t& pos) {
        size_t semicolon = input.find(';', pos);
        if (semicolon == std::string::npos) {
            throw std::runtime_error("Invalid integer format");
        }
        int result = std::stoi(input.substr(pos, semicolon - pos));
        pos = semicolon + 1;
        return result;
    }

    static double parseDouble(const std::string& input, size_t& pos) {
        size_t semicolon = input.find(';', pos);
        if (semicolon == std::string::npos) {
            throw std::runtime_error("Invalid double format");
        }
        double result = std::stod(input.substr(pos, semicolon - pos));
        pos = semicolon + 1;
        return result;
    }

    static bool parseBoolean(const std::string& input, size_t& pos) {
        if (input[pos] == '0' || input[pos] == '1') {
            bool result = input[pos] == '1';
            pos += 2;
            return result;
        }
        throw std::runtime_error("Invalid boolean format");
    }

    static json parseArray(const std::string& input, size_t& pos) {
        size_t colon = input.find(':', pos);
        if (colon == std::string::npos) {
            throw std::runtime_error("Invalid array format");
        }
        int size = std::stoi(input.substr(pos, colon - pos));
        pos = colon + 2;
        json result = json::object();
        for (int i = 0; i < size; ++i) {
            json key = parseValue(input, pos);
            json value = parseValue(input, pos);
            if (key.is_number()) {
                if (!result.is_array()) {
                    json temp = json::array();
                    for (auto& item : result.items()) {
                        temp[item.key()] = item.value();
                    }
                    result = temp;
                }
                result[key.get<int>()] = value;
            } else {
                result[key.get<std::string>()] = value;
            }
        }
        pos++;
        return result;
    }

    // Estimate serialized string size for pre-allocation
    static size_t estimateSerializedSize(const json& j) {
        size_t size = 0;
        if (j.is_null()) {
            size += 2; // "N;"
        } else if (j.is_boolean()) {
            size += 4; // "b:0;" or "b:1;"
        } else if (j.is_number_integer()) {
            size += 12; // "i:" + up to 10 digits + ";"
        } else if (j.is_number_float()) {
            size += 24; // "d:" + up to 22 chars for double + ";"
        } else if (j.is_string()) {
            std::string s = j.get<std::string>();
            size += s.length() + 16; // "s:" + length digits + ":\"\";" + string
        } else if (j.is_array() || j.is_object()) {
            size += 16; // "a:" + size digits + ":{}"
            for (auto& item : j.items()) {
                size += estimateSerializedSize(item.key());
                size += estimateSerializedSize(item.value());
            }
        }
        return size;
    }

    // Serialize JSON value to PHP serialized format using std::string
    static void serializeValue(const json& j, std::string& result) {
        if (j.is_null()) {
            result += "N;";
        } else if (j.is_boolean()) {
            result += "b:";
            result += j.get<bool>() ? "1" : "0";
            result += ";";
        } else if (j.is_number_integer()) {
            result += "i:";
            result += std::to_string(j.get<int>());
            result += ";";
        } else if (j.is_number_float()) {
            result += "d:";
            result += std::to_string(j.get<double>());
            result += ";";
        } else if (j.is_string()) {
            std::string s = j.get<std::string>();
            result += "s:";
            result += std::to_string(s.length());
            result += ":\"";
            result += s;
            result += "\";";
        } else if (j.is_array() || j.is_object()) {
            result += "a:";
            result += std::to_string(j.size());
            result += ":{";
            for (auto& item : j.items()) {
                if (j.is_array()) {
                    serializeValue(json(std::stoi(item.key())), result);
                } else {
                    serializeValue(item.key(), result);
                }
                serializeValue(item.value(), result);
            }
            result += "}";
        } else {
            throw std::runtime_error("Unsupported JSON type");
        }
    }
};

// Example usage
/*
int main() {
    try {
        std::string php_str = "a:2:{s:3:\"key\";s:5:\"value\";i:1;d:42.5;}";
        json result = PhpSerializer::unserialize(php_str);
        std::cout << "Unserialized: " << result.dump(2) << std::endl;

        std::string serialized = PhpSerializer::serialize(result);
        std::cout << "Serialized: " << serialized << std::endl;

        json base = PhpSerializer::unserialize("a:2:{s:3:\"key\";s:5:\"value\";s:4:\"nest\";a:1:{s:2:\"in\";i:1;}}");
        json replacement = PhpSerializer::unserialize("a:2:{s:3:\"key\";s:3:\"new\";s:4:\"nest\";a:1:{s:2:\"in\";i:2;}}");
        json merged = PhpSerializer::array_replace_recursive(base Agilent, replacement);
        std::cout << "Merged: " << merged.dump(2) << std::endl;
        std::cout << "Serialized merged: " << PhpSerializer::serialize(merged) << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    return 0;
}
*/

// 递归序列化函数
std::string Php_Serialize(const json& j) {
    return PhpSerializer::serialize(j);
}

nlohmann::json Php_UnSerialize(const std::string& input) {
    return PhpSerializer::unserialize(input);
}

