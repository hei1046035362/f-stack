#pragma once

#include <iostream>
#include <string>
#include <openssl/aes.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/sha.h>
#include <iomanip>
#include <rte_log.h>

// 初始化OpenSSL库
void initOpenSSL();

// 常用加解密方法

class Encrypt {
public:
    Encrypt(const std::string& key = "c9VTsHTDlTkZv&41", const std::string& iv= "9*4be&k7ec%x9b1t", bool base64 = true)
    : key(key), iv(iv), base64(base64) {}
    ~Encrypt(){}

public:
    static std::string hex2bin(const std::string& hex);

    static std::string bin2hex(const std::string& input);

    static std::string sha1(const std::string& input);

    // 显示sha1的值
    static std::string view_sha1(std::string sha1);


    static std::string Base64Encode(const std::string& input);

    static std::string Base64Decode(const std::string& input);

    std::string Aes128Encrypt(const std::string& origData);

    std::string encrypt(const std::string& data);

    std::string Aes128Decrypt(const std::string& crypted);

private:
    std::string key;
    std::string iv;
    bool base64;
private:
    bool Prepared();
};

// 模拟defaultEncrypt函数
Encrypt GetEncryptor(const std::string& key = "c9VTsHTDlTkZv&41", const std::string& iv = "9*4be&k7ec%x9b1t");


