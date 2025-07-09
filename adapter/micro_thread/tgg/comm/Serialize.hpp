#pragma once

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <string>


// 递归序列化函数
std::string Php_Serialize(const rapidjson::Value& j);

rapidjson::Document Php_UnSerialize(const std::string& input);

rapidjson::Document Php_ArrayReplaceRecursive(const rapidjson::Value& base, const rapidjson::Value& replacement, rapidjson::Document::AllocatorType& allocator);
