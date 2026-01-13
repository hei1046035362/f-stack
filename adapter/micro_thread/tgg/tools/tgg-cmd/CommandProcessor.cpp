#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <rte_hash.h>
#include <rte_hash_crc.h>
#include "comm/MurmurHash3.h"
#include "dpdk_init.h"
#include "comm/log.hpp"
using namespace std;

extern struct rte_memzone* g_random_zone;
extern int g_run;

// 命令处理类
class CommandProcessor {
private:
    map<string, function<void()>> commands;
    map<string, string> commandDescriptions;
    bool running = true;
    
    // 辅助函数：转换为小写
    string toLower(const string& str) {
        string result = str;
        transform(result.begin(), result.end(), result.begin(), ::tolower);
        return result;
    }
    
    // 辅助函数：去除首尾空格
    string trim(const string& str) {
        size_t first = str.find_first_not_of(' ');
        if (string::npos == first) {
            return str;
        }
        size_t last = str.find_last_not_of(' ');
        return str.substr(first, (last - first + 1));
    }
    
    // 辅助函数：分割字符串
    vector<string> split(const string& str, char delimiter) {
        vector<string> tokens;
        string token;
        istringstream tokenStream(str);
        while (getline(tokenStream, token, delimiter)) {
            token = trim(token);
            if (!token.empty()) {
                tokens.push_back(token);
            }
        }
        return tokens;
    }
    
public:
    // 构造函数，注册所有命令
    CommandProcessor() {
        registerCommands();
    }
    
    // 注册所有命令
    void registerCommands() {
        // 帮助命令
        registerCommand("help", "显示所有可用命令", [this]() { showHelp(); });
        registerCommand("?", "显示所有可用命令", [this]() { showHelp(); });
        
        // 系统命令
        registerCommand("exit", "退出程序", [this]() { exitCommand(); });
        registerCommand("quit", "退出程序", [this]() { exitCommand(); });
        registerCommand("clear", "清屏", [this]() { clearScreen(); });
        
        // 示例功能命令
        registerCommand("hash", "hash生成器", [this]() { printHashSig(); });
        // registerCommand("time", "显示当前时间", [this]() { showTime(); });
        // registerCommand("date", "显示当前日期", [this]() { showDate(); });
        // registerCommand("echo", "回显输入内容", [this]() { echoCommand(); });
        // registerCommand("calc", "简单计算器", [this]() { calculator(); });
        // registerCommand("info", "显示系统信息", [this]() { showSystemInfo(); });
        // registerCommand("version", "显示版本信息", [this]() { showVersion(); });
        // registerCommand("config", "显示当前配置", [this]() { showConfig(); });
    }
    
    // 注册单个命令
    void registerCommand(const string& cmd, const string& description, function<void()> handler) {
        string lowerCmd = toLower(cmd);
        commands[lowerCmd] = handler;
        commandDescriptions[lowerCmd] = description;
    }
    
    // 显示帮助信息
    void showHelp() {
        cout << "\n======================== 可用命令 ========================\n";
        cout << "格式: 命令 [参数1] [参数2] ...\n\n";
        
        // 按命令分组显示
        map<string, vector<string>> groupedCommands;
        
        for (const auto& pair : commandDescriptions) {
            if (pair.first == "help" || pair.first == "?") {
                groupedCommands["帮助命令"].push_back(pair.first);
            } else if (pair.first == "exit" || pair.first == "quit" || pair.first == "clear") {
                groupedCommands["系统命令"].push_back(pair.first);
            } else {
                groupedCommands["功能命令"].push_back(pair.first);
            }
        }
        
        for (const auto& group : groupedCommands) {
            cout << "[" << group.first << "]:\n";
            for (const auto& cmd : group.second) {
                cout << "  " << cmd;
                // 添加适当的空格对齐
                int spaces = 12 - cmd.length();
                if (spaces < 0) spaces = 0;
                cout << string(spaces, ' ') << " - " << commandDescriptions[cmd] << endl;
            }
            cout << endl;
        }
        
        cout << "========================================================\n";
        cout << "输入 'exit' 或 'quit' 退出程序\n";
        cout << "输入 'clear' 清屏\n";
        cout << "========================================================\n\n";
    }
    
    // 执行命令
    bool executeCommand(const string& input) {
        if (input.empty()) {
            return true;
        }
        
        // 分割输入
        vector<string> tokens = split(input, ' ');
        if (tokens.empty()) {
            return true;
        }
        
        string cmd = toLower(tokens[0]);
        
        // 检查是否是带参数的帮助命令
        if (cmd == "help" && tokens.size() > 1) {
            showCommandHelp(tokens[1]);
            return true;
        }
        
        // 查找并执行命令
        auto it = commands.find(cmd);
        if (it != commands.end()) {
            try {
                it->second();  // 执行对应的函数
            } catch (const exception& e) {
                cout << "执行命令时发生错误: " << e.what() << endl;
            }
            return true;
        } else {
            cout << "\n错误: 未知命令 '" << cmd << "'\n";
            cout << "输入 'help' 查看可用命令列表\n";
            return false;
        }
    }
    
    // 显示特定命令的帮助
    void showCommandHelp(const string& cmd) {
        string lowerCmd = toLower(cmd);
        auto it = commandDescriptions.find(lowerCmd);
        if (it != commandDescriptions.end()) {
            cout << "\n命令: " << lowerCmd << endl;
            cout << "描述: " << it->second << endl;
            
            // 显示别名
            vector<string> aliases;
            for (const auto& pair : commandDescriptions) {
                if (pair.second == it->second && pair.first != lowerCmd) {
                    aliases.push_back(pair.first);
                }
            }
            
            if (!aliases.empty()) {
                cout << "别名: ";
                for (size_t i = 0; i < aliases.size(); i++) {
                    cout << aliases[i];
                    if (i < aliases.size() - 1) cout << ", ";
                }
                cout << endl;
            }
        } else {
            cout << "没有找到命令 '" << cmd << "' 的帮助信息\n";
        }
    }
    
    // 运行主循环
    void run() {
        cout << "========================================================" << endl;
        cout << "               命令行交互程序 v1.0" << endl;
        cout << "========================================================" << endl;
        showHelp();
        
        string input;
        while (running && g_run) {
            cout << ">> ";
            getline(cin, input);
            
            if (!executeCommand(input)) {
                // 如果命令执行失败，显示简化的帮助
                cout << "可用命令: ";
                int count = 0;
                for (const auto& pair : commandDescriptions) {
                    if (count > 0) cout << ", ";
                    cout << pair.first;
                    count++;
                    if (count >= 8) {  // 每行显示8个命令
                        cout << "\n           ";
                        count = 0;
                    }
                }
                cout << endl;
            }
        }
    }
    
    // ================ 具体命令的实现 ================
    
    void printHashSig()
    {
        if(!g_random_zone) {
            cout << "g_random_zone not found.\n";
            return;
        }
        uint32_t random = *((uint32_t*)(g_random_zone->addr));
        cout << "\n======== hash生成器，gid,uid等的hash ========\n";
        cout << "输入 'back' 返回主菜单\n";
        cout << "hash seed:["<< random << "]\n";
        cout << "===========================\n";
        
        while (true) {
            cout << "生成hash >> ";
            string expr;
            getline(cin, expr);
            
            if (toLower(expr) == "back" || toLower(expr) == "exit") {
                cout << "退出生成器\n";
                break;
            }
            uint64_t sig = murmurhash3_64(expr.c_str(), expr.length(), random);
            cout << "\tdata:[" << expr << "]\n";
            cout << "\thash:[" << sig << "]\n";
        }
    }

    void exitCommand() {
        cout << "确定要退出吗？(y/n): ";
        string confirm;
        getline(cin, confirm);
        if (toLower(confirm) == "y" || toLower(confirm) == "yes") {
            cout << "再见！\n";
            running = false;
        } else {
            cout << "取消退出\n";
        }
    }
    
    void clearScreen() {
        // 简单的清屏实现
        for (int i = 0; i < 50; i++) {
            cout << endl;
        }
        cout << "屏幕已清空\n";
    }
    
    // void sayHello() {
    //     cout << "你好！欢迎使用命令行交互程序！\n";
    // }
    
    // void showTime() {
    //     time_t now = time(nullptr);
    //     tm* localTime = localtime(&now);
    //     cout << "当前时间: " << asctime(localTime);
    // }
    
    // void showDate() {
    //     time_t now = time(nullptr);
    //     tm* localTime = localtime(&now);
    //     char buffer[80];
    //     strftime(buffer, sizeof(buffer), "日期: %Y年%m月%d日", localTime);
    //     cout << buffer << endl;
        
    //     // 显示星期几
    //     const char* weekdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    //     cout << "星期: " << weekdays[localTime->tm_wday] << endl;
    // }
    
    // void echoCommand() {
    //     cout << "请输入要回显的内容: ";
    //     string text;
    //     getline(cin, text);
    //     cout << "回显: " << text << endl;
    // }
    
    // void calculator() {
    //     cout << "\n======== 简单计算器 ========\n";
    //     cout << "支持操作: +, -, *, /, %\n";
    //     cout << "示例: 3 + 4\n";
    //     cout << "输入 'back' 返回主菜单\n";
    //     cout << "===========================\n";
        
    //     while (true) {
    //         cout << "计算器 >> ";
    //         string expr;
    //         getline(cin, expr);
            
    //         if (toLower(expr) == "back" || toLower(expr) == "exit") {
    //             cout << "退出计算器\n";
    //             break;
    //         }
            
    //         double num1, num2;
    //         char op;
    //         istringstream iss(expr);
            
    //         if (iss >> num1 >> op >> num2) {
    //             double result = 0;
    //             bool valid = true;
                
    //             switch (op) {
    //                 case '+':
    //                     result = num1 + num2;
    //                     break;
    //                 case '-':
    //                     result = num1 - num2;
    //                     break;
    //                 case '*':
    //                     result = num1 * num2;
    //                     break;
    //                 case '/':
    //                     if (num2 != 0) {
    //                         result = num1 / num2;
    //                     } else {
    //                         cout << "错误: 除数不能为0\n";
    //                         valid = false;
    //                     }
    //                     break;
    //                 case '%':
    //                     if (static_cast<int>(num2) != 0) {
    //                         result = static_cast<int>(num1) % static_cast<int>(num2);
    //                     } else {
    //                         cout << "错误: 除数不能为0\n";
    //                         valid = false;
    //                     }
    //                     break;
    //                 default:
    //                     cout << "错误: 不支持的操作符 '" << op << "'\n";
    //                     valid = false;
    //             }
                
    //             if (valid) {
    //                 cout << "结果: " << result << endl;
    //             }
    //         } else {
    //             cout << "错误: 无效的表达式格式\n";
    //             cout << "正确格式: 数字 操作符 数字\n";
    //         }
    //     }
    // }
    
    // void showSystemInfo() {
    //     cout << "\n======== 系统信息 ========\n";
    //     // 这里可以添加获取实际系统信息的代码
    //     cout << "程序名称: 命令行交互程序\n";
    //     cout << "运行状态: 正常\n";
    //     cout << "已注册命令数: " << commands.size() << endl;
    //     cout << "运行时间: " << time(nullptr) << " 秒\n";
    //     cout << "=========================\n";
    // }
    
    // void showVersion() {
    //     cout << "\n======== 版本信息 ========\n";
    //     cout << "命令行交互程序 v1.0.0\n";
    //     cout << "编译时间: " << __DATE__ << " " << __TIME__ << endl;
    //     cout << "作者: 命令行工具开发者\n";
    //     cout << "许可证: MIT\n";
    //     cout << "=========================\n";
    // }
    
    // void showConfig() {
    //     cout << "\n======== 当前配置 ========\n";
    //     cout << "命令处理器: 已启用\n";
    //     cout << "命令自动补全: 未实现\n";
    //     cout << "历史记录: 未实现\n";
    //     cout << "命令别名: 已支持\n";
    //     cout << "=========================\n";
    // }
};