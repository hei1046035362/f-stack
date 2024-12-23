#pragma once

#include <nlohmann/json.hpp>
#include <string>


// 递归序列化函数
std::string Php_Serialize(const nlohmann::json& j);

nlohmann::json Php_UnSerialize(const std::string& input);