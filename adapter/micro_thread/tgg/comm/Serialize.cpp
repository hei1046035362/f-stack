#include "Serialize.hpp"
#include <iostream>
#include <sstream>

using json = nlohmann::json;

// 递归序列化函数
std::string Php_Serialize(const json& j) {
    std::stringstream ss;

    if (j.is_object()) {
        // 对象 -> PHP关联数组格式
        ss << "a:" << j.size() << ":{";
        for (auto it = j.begin(); it != j.end(); ++it) {
            ss << "s:" << it.key().size() << ":\"" << it.key() << "\";" 
               << Php_Serialize(it.value());
        }
        ss << "}";
    } else if (j.is_array()) {
        // 数组 -> PHP索引数组格式
        ss << "a:" << j.size() << ":{";
        for (size_t i = 0; i < j.size(); ++i) {
            ss << "i:" << i << ";" << Php_Serialize(j[i]);
        }
        ss << "}";
    } else if (j.is_string()) {
        // 字符串 -> PHP字符串格式
        ss << "s:" << j.get<std::string>().size() << ":\"" << j.get<std::string>() << "\";";
    } else if (j.is_number_integer()) {
        // 整数 -> PHP整数格式
        ss << "i:" << j.get<int>() << ";";
    } else if (j.is_number_float()) {
        // 浮点数 -> PHP浮点数格式
        ss << "d:" << j.get<double>() << ";";
    } else if (j.is_boolean()) {
        // 布尔值 -> PHP布尔值格式
        ss << "b:" << (j.get<bool>() ? 1 : 0) << ";";
    } else if (j.is_null()) {
        // 空值 -> PHP NULL
        ss << "N;";
    } else {
        throw std::invalid_argument("Unsupported JSON type for PHP serialization.");
    }

    return ss.str();
}

nlohmann::json Php_UnSerialize(const std::string& input) {
    size_t pos = 0;//input.find('{') + 1;
    
    auto skipWhitespace = [&]() {
        while (pos < input.length() && std::isspace(input[pos])) pos++;
    };
    
    auto parseStringLength = [&]() {
        skipWhitespace();
        size_t lenEnd = input.find(':', pos);// 取字符串的长度(字符串)或元素个数(对象)
        if (lenEnd == std::string::npos) return (size_t)0;
        
        size_t len = std::stoi(input.substr(pos, lenEnd - pos));
        pos = lenEnd + 2; // 跳过 ":"
        return len;
    };

    auto parseString = [&](size_t len) {
        if (pos + len > input.length()) return std::string();
        std::string str = input.substr(pos, len);
        pos += len + 2; // 跳过字符串和引号
        return str;
    };

    std::function<nlohmann::json()> parseValue = [&]() {
        skipWhitespace();
        
        // 字符串
        if (input[pos] == 's') {
            pos += 2;
            size_t len = parseStringLength();
            return nlohmann::json(parseString(len));
        }
        // 整数
        else if (input[pos] == 'i') {
            pos += 2;
            size_t end = input.find(';', pos);
            int val = std::stoi(input.substr(pos, end - pos));
            pos = end + 1;
            return nlohmann::json(val);
        }
        // 浮点数
        else if (input[pos] == 'd') {
            pos += 2;
            size_t end = input.find(';', pos);
            double val = std::stod(input.substr(pos, end - pos));
            pos = end + 1;
            return nlohmann::json(val);
        }
        // 布尔值
        else if (input[pos] == 'b') {
            pos += 2;
            bool val = (input[pos] == '1');
            pos += 2;
            return nlohmann::json(val);
        }
        // Null
        else if (input[pos] == 'N') {
            pos += 2;
            return nlohmann::json(nullptr);
        }
        // 数组
        else if (input[pos] == 'a') {
            pos += 2;
            size_t arrayLen = parseStringLength();
            
            nlohmann::json arr = arrayLen == 1 ? nlohmann::json::object() : nlohmann::json::array();
            
            for (size_t i = 0; i < arrayLen; ++i) {
                skipWhitespace();
                
                // 解析键
                nlohmann::json key;
                if (input[pos] == 'i') {
                    pos += 2;
                    size_t end = input.find(';', pos);
                    int nkey = std::stoi(input.substr(pos, end - pos));
                    key = nkey;
                    pos = end + 1;
                    arr = nlohmann::json::array();
                }
                else if (input[pos] == 's') {
                    pos += 2;
                    size_t len = parseStringLength();
                    std::string skey = parseString(len);
                    key = skey;
                }
                
                // 解析值
                nlohmann::json value = parseValue();
                
                if (arr.is_object()) {
                    std::string sKey = key.dump();
                    sKey.erase(std::remove(sKey.begin(), sKey.end(), '\"'), sKey.end());
                    arr[sKey] = value;
                } else {
                    if (!key.is_null() && !key.is_number_integer()) {// key 为整数说明是数组下标，下标不需要保存
                        nlohmann::json obj;
                        std::string sKey = key.dump();
                        sKey.erase(std::remove(sKey.begin(), sKey.end(), '\"'), sKey.end());
                        obj[sKey] = value;
                        arr.push_back(obj);
                    } else {
                        arr.push_back(value);
                    }
                }
                // std::cout << "key:" << key.dump() << std::endl;
                // std::cout << "value:" << value.dump() << std::endl;
                // std::cout << "array:" << arr.dump(4) << std::endl;
            }
            
            pos++; // 跳过 }
            return arr;
        }
        
        throw std::runtime_error("Unknown type at position " + std::to_string(pos));
    };
    
    return parseValue();
}

// int main() {
//     // 构造 JSON 数据
//     json data = {
//         {"487336", {
//             {"groups", json::array({"416078112896319488"})},
//             {"uid", "416078112896319488"}
//         }}
//     };

//     // 序列化为 PHP 格式
//     std::string php_serialized = Php_Serialized(data);
//     std::cout << "PHP Serialized Data:\n" << php_serialized << std::endl;
//     // std::string serialized_data = "a:1:{s:6:\"487336\";a:2:{s:6:\"groups\";a:1:{i:0;s:18:\"416078112896319488\";}s:3:\"uid\";s:18:\"416078112896319488\";}";
//     std::string phpSerialized = "a:1:{s:6:\"487336\";a:2:{s:6:\"groups\";a:1:{i:0;s:18:\"416078112896319488\";}s:3:\"uid\";s:18:\"416078112896319488\";}}";
    
//     try {
        
//         nlohmann::json result = Php_UnSerialize(phpSerialized);
        
//         std::cout << result.dump(4) << std::endl;
//     } 
//     catch (const std::exception& e) {
//         std::cerr << "Parse error: " << e.what() << std::endl;
//     }
//     return 0;
// }
