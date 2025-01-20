#include <string>
#include <vector>
#include <map>
#include <mutex>



class IniFileHandler {
private:
    map<string, map<string, string>> sections;  // 存储INI文件的节以及节内的键值对，外层map以节名作为键
    mutex mtx;  // 互斥锁，用于保护共享数据sections的访问

public:
    // 从文件中读取INI文件内容并解析
    bool readFromFile(const string& filename);

    // 根据节名和键名获取对应的值，如果节或键不存在则返回空字符串
    string getValue(const string& section, const string& key);

    // 向指定节中添加键值对，如果节不存在则创建新节
    void addKeyValue(const string& section, const string& key, const string& value);

    // 将当前INI文件对象的内容写入到指定的文件中
    bool writeToFile(const string& filename);
};