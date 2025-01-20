#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include "RWIni.hpp"
using namespace std;

bool IniFileHandler::readFromFile(const string& filename) {
    unique_lock<mutex> lock(mtx);  // 在读取文件操作前加锁，确保线程安全
    ifstream file(filename);
    if (!file) {
        cerr << "open file for read fialed: " << filename << endl;
        return false;
    }
    string line;
    string currentSection;
    while (getline(file, line)) {
        // 跳过 空行、注释 
        if (line.empty() || line.front() == '#' || line.find_first_not_of(' ') == std::string::npos) continue;
        // 去除两端空白字符
        line = line.substr(line.find_first_not_of(' '), line.find_last_not_of(' ')+1);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.length() - 2);
            sections[currentSection] = map<string, string>();
        } else {
            size_t pos = line.find('=');
            if (pos!= string::npos) {
                // key
                string key = line.substr(0, pos);
                if (key.find_first_not_of(' ') == std::string::npos) continue;
                key = key.substr(key.find_first_not_of(' '), key.find_last_not_of(' ')+1);
                if (key.empty()) continue;
                // value
                string value = line.substr(pos + 1);
                if (value.find_first_not_of(' ') != std::string::npos)
                    value = value.substr(value.find_first_not_of(' '), value.find_last_not_of(' ')+1);
                else
                    value = "";
                // 赋值
                if (!currentSection.empty()) {
                    sections[currentSection][key] = value;
                }
            }
        }
    }
    file.close();
    return true;
}
// 根据节名和键名获取对应的值，如果节或键不存在则返回空字符串
string IniFileHandler::getValue(const string& section, const string& key) {
    unique_lock<mutex> lock(mtx);  // 在获取值操作前加锁，确保线程安全
    auto sectionIt = sections.find(section);
    if (sectionIt!= sections.end()) {
        auto keyIt = sectionIt->second.find(key);
        if (keyIt!= sectionIt->second.end()) {
            return keyIt->second;
        }
    }
    return "";
}

// 向指定节中添加键值对，如果节不存在则创建新节
void IniFileHandler::addKeyValue(const string& section, const string& key, const string& value) {
    unique_lock<mutex> lock(mtx);  // 在添加键值对操作前加锁，确保线程安全
    if (key.empty())
        return;
    sections[section][key] = value;
}

// 将当前INI文件对象的内容写入到指定的文件中
bool IniFileHandler::writeToFile(const string& filename) {
    unique_lock<mutex> lock(mtx);  // 在写入文件操作前加锁，确保线程安全
    ofstream file(filename);
    if (!file) {
        cerr << "open file for write failed: " << filename << endl;
        return false;
    }
    for (const auto& section : sections) {
        file << "[" << section.first << "]" << endl;
        for (const auto& pair : section.second) {
            file << pair.first << " = " << pair.second << endl;
        }
        file << endl;
    }
    file.close();
    return true;
}
