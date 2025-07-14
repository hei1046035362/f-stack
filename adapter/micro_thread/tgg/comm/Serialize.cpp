#include "Serialize.hpp"
#include <stdexcept>
#include "log.hpp"
using namespace rapidjson;

class PhpSerializer {
public:
    // Unserialize PHP serialized string to RapidJSON Document
    static Document unserialize(const std::string& input) {
        size_t pos = 0;
        Document doc;
        doc.SetObject(); // 初始化为对象
        parseValue(input, pos, doc, doc.GetAllocator());
        return doc;
    }

    // Serialize RapidJSON Value to PHP serialized string
    static std::string serialize(const Value& j) {
        std::string result;
        serializeValue(j, result);
        return result;
    }

    // PHP's array_replace_recursive equivalent
    static void recursive_merge(Value& base, const Value& replacement, Document::AllocatorType& allocator) {
        if (base.IsObject() && replacement.IsObject()) {
            for (auto& m : replacement.GetObject()) {
                const char* key = m.name.GetString();
                Value::MemberIterator baseIt = base.FindMember(key);
                if (baseIt != base.MemberEnd()) {
                    // 递归合并对象
                    if (baseIt->value.IsObject() && m.value.IsObject()) {
                        recursive_merge(baseIt->value, m.value, allocator);
                    } 
                    // 直接覆盖非对象值（避免深拷贝）
                    else {
                        baseIt->value.CopyFrom(m.value, allocator);
                    }
                } 
                // 新增键值对（仅浅拷贝键名）
                else {
                    Value newKey(key, allocator); 
                    Value newValue;
                    newValue.CopyFrom(m.value, allocator); // 深拷贝到临时对象
                    base.AddMember(newKey, newValue.Move(), allocator); // 使用Move转移所有权
                }
            }
        }
    }

    static Document array_replace_recursive(const Value& base, const Value& replacement, Document::AllocatorType& allocator) {
        Document result;
        result.CopyFrom(base, allocator); // 仅此一次深拷贝
    
        // 递归合并逻辑（原地修改，避免临时对象）
        if (replacement.IsObject() && result.IsObject()) {
            recursive_merge(result, replacement, allocator);
        } else {
            result.CopyFrom(replacement, allocator); // 非对象直接覆盖
        }
        return result;
    }

private:
    // Parse a single value from PHP serialized string
    static void parseValue(const std::string& input, size_t& pos, Value& parent, Document::AllocatorType& allocator) {
        if (pos >= input.length()) {
            throw std::runtime_error("Unexpected end of input");
        }

        char type = input[pos];
        pos += 2; // Skip type and colon

        switch (type) {
            case 's': parseString(input, pos, parent, allocator); break;
            case 'i': parseInteger(input, pos, parent, allocator); break;
            case 'd': parseDouble(input, pos, parent, allocator); break;
            case 'b': parseBoolean(input, pos, parent, allocator); break;
            case 'a': parseArray(input, pos, parent, allocator); break;
            case 'N': pos++; parent.SetNull(); break;
            default: throw std::runtime_error("Unknown type: " + std::string(1, type));
        }
    }

    static void parseString(const std::string& input, size_t& pos, Value& parent, Document::AllocatorType& allocator) {
        size_t colon = input.find(':', pos);
        if (colon == std::string::npos) throw std::runtime_error("Invalid string format");
        
        int length = std::stoi(input.substr(pos, colon - pos));
        pos = colon + 2; // Skip colon and quote
        std::string str = input.substr(pos, length);
        pos += length + 2; // Skip string and closing quote
        
        parent.SetString(str.c_str(), static_cast<SizeType>(str.length()), allocator);
    }

    static void parseInteger(const std::string& input, size_t& pos, Value& parent, Document::AllocatorType& allocator) {
        size_t semicolon = input.find(';', pos);
        if (semicolon == std::string::npos) throw std::runtime_error("Invalid integer format");
        
        int value = std::stoi(input.substr(pos, semicolon - pos));
        pos = semicolon + 1;
        parent.SetInt(value);
    }

    static void parseDouble(const std::string& input, size_t& pos, Value& parent, Document::AllocatorType& allocator) {
        size_t semicolon = input.find(';', pos);
        if (semicolon == std::string::npos) throw std::runtime_error("Invalid double format");
        
        double value = std::stod(input.substr(pos, semicolon - pos));
        pos = semicolon + 1;
        parent.SetDouble(value);
    }

    static void parseBoolean(const std::string& input, size_t& pos, Value& parent, Document::AllocatorType& allocator) {
        if (input[pos] == '0' || input[pos] == '1') {
            parent.SetBool(input[pos] == '1');
            pos += 2;
        } else {
            throw std::runtime_error("Invalid boolean format");
        }
    }

    static void parseArray(const std::string& input, size_t& pos, Value& parent, Document::AllocatorType& allocator) {
        size_t colon = input.find(':', pos);
        if (colon == std::string::npos) throw std::runtime_error("Invalid array format");
        
        int size = std::stoi(input.substr(pos, colon - pos));
        pos = colon + 2; // Skip colon and opening brace
        
        parent.SetObject(); // PHP数组在JSON中表示为对象
        
        for (int i = 0; i < size; ++i) {
            Value key;
            parseValue(input, pos, key, allocator); // 解析键
            
            Value value;
            parseValue(input, pos, value, allocator); // 解析值
            
            // 添加键值对
            if (key.IsString()) {
                parent.AddMember(
                    Value(key.GetString(), allocator),
                    value,
                    allocator
                );
            } else if (key.IsInt()) {
                // 数字键转换为字符串键（保持PHP数组特性）
                std::string numKey = std::to_string(key.GetInt());
                parent.AddMember(
                    Value(numKey.c_str(), allocator),
                    value,
                    allocator
                    );
            }
        }
        pos++; // Skip closing brace
    }

    // Serialize RapidJSON Value to PHP format
    static void serializeValue(const rapidjson::Value& j, std::string& result) {
        if (j.IsNull()) {
            result += "N;";
        } else if (j.IsBool()) {
            result += "b:";
            result += j.GetBool() ? "1" : "0";
            result += ";";
        } else if (j.IsInt()) {
            result += "i:";
            result += std::to_string(j.GetInt());
            result += ";";
        } else if (j.IsUint()) {
            result += "i:";
            result += std::to_string(j.GetUint());
            result += ";";
        } else if (j.IsInt64()) {
            result += "i:";
            result += std::to_string(j.GetInt64());
            result += ";";
        } else if (j.IsUint64()) {
            result += "i:";
            result += std::to_string(j.GetUint64());
            result += ";";
        } else if (j.IsDouble()) {
            result += "d:";
            result += std::to_string(j.GetDouble());
            result += ";";
        } else if (j.IsString()) {
            std::string s = j.GetString();
            result += "s:";
            result += std::to_string(s.length());
            result += ":\"";
            result += s;
            result += "\";";
        } else if (j.IsArray()) {  // 新增数组类型处理
            result += "a:";
            result += std::to_string(j.Size());  // 数组元素数量
            result += ":{";
        
            // 遍历数组元素
            for (rapidjson::SizeType i = 0; i < j.Size(); ++i) {
                // 序列化数组索引（PHP要求索引从0开始）
                result += "i:";
                result += std::to_string(i);
                result += ";";
                
                // 序列化数组元素值
                serializeValue(j[i], result);
            }
            result += "}";
        } else if (j.IsObject()) {
            result += "a:";
            result += std::to_string(j.MemberCount());
            result += ":{";
        
            for (auto& m : j.GetObject()) {
                // 序列化键（修复潜在类型问题）
                if (m.name.IsString()) {
                    // 安全处理字符串键
                    result += "s:";
                    result += std::to_string(m.name.GetStringLength());
                    result += ":\"";
                    result += m.name.GetString();
                    result += "\";";
                } else {
                    // 非字符串键直接序列化
                    serializeValue(m.name, result);
                }
            
                // 序列化值
                serializeValue(m.value, result);
            }
            result += "}";
        } else {
            LOG_ERROR("Unknown value type:%d", j.GetType());
            // 处理未知类型（安全回退）
            result += "N;";
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
std::string Php_Serialize(const Value& j) {
    return PhpSerializer::serialize(j);
}

Document Php_UnSerialize(const std::string& input) {
    return PhpSerializer::unserialize(input);
}

Document Php_ArrayReplaceRecursive(const Value& base, const Value& replacement, Document::AllocatorType& allocator) {
    return PhpSerializer::array_replace_recursive(base, replacement, allocator);
}