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
static void initOpenSSL() {
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
}

// 常用加解密方法

class Encrypt {
public:
    Encrypt(const std::string& key = "c9VTsHTDlTkZv&41", const std::string& iv= "9*4be&k7ec%x9b1t", bool base64 = true)
    : key(key), iv(iv), base64(base64) {}
    ~Encrypt(){}

public:
    static std::string hex2bin(const std::string& hex)
    {
        if (hex.length() % 2 != 0) {
            RTE_LOG(ERR, USER1, "[%s][%d] Hex string must have an even length.", 
                __func__, __LINE__);
            return "";
        }

        std::string binary;
        for (size_t i = 0; i < hex.length(); i += 2) {
            // 提取两个字符
            std::string byteString = hex.substr(i, 2);
            // 转换成整数
            char byte = static_cast<char>(strtol(byteString.c_str(), nullptr, 16));
            binary.push_back(byte); // 添加到结果字符串
        }
    
        return binary;
    }

    static std::string bin2hex(const std::string& input)
    {
        std::stringstream ss;

        for (unsigned char c : input) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }

        return ss.str();
    }

    static std::string sha1(const std::string& input)
    {
        // 计算编码后大致需要的缓冲区大小
        std::string sha1data;
        sha1data.resize(SHA_DIGEST_LENGTH);
        // 把sha1转换成字符串
        SHA1(reinterpret_cast<const unsigned char *>(input.data()), input.size(), (unsigned char*)sha1data.c_str());
        return sha1data;
    }

    // 显示sha1的值
    static std::string view_sha1(std::string sha1)
    {
        std::string sSha1;
        sSha1.resize(SHA_DIGEST_LENGTH * 2);
        for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
            sprintf(sSha1.data() + i*2, "%02x", (unsigned char)(*(sha1.c_str() + i)));
        }
        return sSha1;
    }


    static std::string Base64Encode(const std::string& input)
    {
        // 计算编码后大致需要的缓冲区大小
        int encodedLen = 4 * ((input.size() + 2) / 3) + 4;
        std::string encryptedData;
        encryptedData.resize(encodedLen);

        // 进行Base64编码
        int result = EVP_EncodeBlock((unsigned char*)encryptedData.c_str(),
            reinterpret_cast<const unsigned char *>(input.data()),
            input.size());
        if (result < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d] base64 encode failed,code:%d.", 
                __func__, __LINE__, result);
            return "";
        }
        // 去掉末尾的\0，base64操作之后会留下一些\0，length会包含这些\0，导致length不准确
        return encryptedData.substr(0, result);
    }

    static std::string Base64Decode(const std::string& input)
    {
        // 计算解码后大致需要的缓冲区大小，这里简单估算为编码长度的3/4
        int decodedLen = (input.length() * 3) / 4;
        std::string decryptedData;
        decryptedData.resize(decodedLen);

        // 进行Base64解码
        int result = EVP_DecodeBlock((unsigned char*)decryptedData.c_str(),
            reinterpret_cast<const unsigned char *>(input.data()),
            input.length());
        if (result < 0) {
            RTE_LOG(ERR, USER1, "[%s][%d] base64 decode failed,code:%d.", 
                __func__, __LINE__, result);
            return "";
        }
        return decryptedData;
    }

    std::string Aes128Encrypt(const std::string& origData)
    {
        if (!Prepared()) {
            return "";
        }

        std::string data = origData;
        if (base64) {
            // Base64 encode
            data = Base64Encode(data);
        }

        int blockSize = EVP_CIPHER_iv_length(EVP_aes_128_cbc());

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, (const unsigned char*)key.c_str(), (const unsigned char*)iv.c_str());

        int len, exLen;
        int dataLength = data.length();
        std::string cryptedData;
        cryptedData.resize(dataLength + blockSize);

        EVP_EncryptUpdate(ctx, (unsigned char*)cryptedData.c_str(), &len, (const unsigned char*)data.c_str(), dataLength);
        EVP_EncryptFinal_ex(ctx, (unsigned char*)cryptedData.c_str() + len, &exLen);

        EVP_CIPHER_CTX_free(ctx);
        // cryptedData 的长度要压缩到实际加密结果的长度
        return bin2hex(cryptedData.substr(0, exLen + len));

    }

    std::string encrypt(const std::string& data)
    {
        std::string ret;
        if (!Prepared()) {
            return ret;
        }
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, (const unsigned char*)key.c_str(), (const unsigned char*)iv.c_str());

        int len, exLen;
        int dataLength = data.length();
        std::string encryptedData;
        encryptedData.resize(dataLength + EVP_CIPHER_iv_length(EVP_aes_128_cbc()));

        EVP_EncryptUpdate(ctx, (unsigned char*)encryptedData.c_str(), &len, (const unsigned char*)data.c_str(), dataLength);
        EVP_EncryptFinal_ex(ctx, (unsigned char*)encryptedData.c_str() + len, &exLen);
        EVP_CIPHER_CTX_free(ctx);

        return Base64Encode(encryptedData.substr(0, exLen + len));
    }

    std::string Aes128Decrypt(const std::string& crypted)
    {
        if (!Prepared()) {
            return "";
        }

        int blockSize = EVP_CIPHER_iv_length(EVP_aes_128_cbc());

        // 将十六进制字符串转换为二进制数据
        std::string binData = hex2bin(crypted);

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, (const unsigned char*)key.c_str(), (const unsigned char*)iv.c_str());

        int len, exLen;
        int binDataLength = binData.length();
        std::string decryptedData;
        decryptedData.resize(binDataLength);

        EVP_DecryptUpdate(ctx, (unsigned char*)decryptedData.c_str(), &len, (const unsigned char*)binData.c_str(), binDataLength);
        EVP_DecryptFinal_ex(ctx, (unsigned char*)decryptedData.c_str() + len, &exLen);

        EVP_CIPHER_CTX_free(ctx);
        decryptedData = decryptedData.substr(0, len + exLen);
        if (base64) {
            return Base64Decode(decryptedData);
        }

        return decryptedData;
    }

private:
    std::string key;
    std::string iv;
    bool base64;
private:
    bool Prepared() {
        if (key.empty() || key.length()!= 16) {
            RTE_LOG(ERR, USER1, "[%s][%d] Illigal authorized key:%s.", 
                __func__, __LINE__, key.c_str());
            return false;
        }
        if (!iv.empty() && iv.length()!= 16) {
            RTE_LOG(ERR, USER1, "[%s][%d] Illigal authorized iv:%s.", 
                __func__, __LINE__, iv.c_str());
            return false;
        }
        return true;
    }
};

// 模拟defaultEncrypt函数
Encrypt defaultEncrypt(const std::string& key = "c9VTsHTDlTkZv&41", const std::string& iv = "9*4be&k7ec%x9b1t") {
    return Encrypt(key, iv, true);
}

// int main() {
//     initOpenSSL();
//     Encrypt encryptor = defaultEncrypt();
//     std::string originalData = "Hello, World!";
//     std::string encryptedData = encryptor.Aes128Encrypt(originalData);
//     std::string decryptedData = encryptor.Aes128Decrypt(encryptedData);
//     std::string base64En = encryptor.Base64Encode(originalData);
//     std::cout << "Original: " << originalData << std::endl;
//     std::cout << "Base64Encode: " << base64En << std::endl;
//     std::cout << "Base64Decode: " << encryptor.Base64Decode(base64En) << std::endl;
//     std::cout << "AesEncrypted: " << encryptedData << std::endl;
//     std::cout << "AesDecrypted: " << decryptedData << std::endl;
//     std::cout << "Encrypt: " << encryptor.encrypt(originalData) << std::endl;
//     std::string sha1data = encryptor.sha1(originalData);
//     std::cout << "sha1: " << sha1data << std::endl;
//     std::cout << "sha1str: " << Encrypt::view_sha1(sha1data) << std::endl;
//     std::string _64data = Encrypt::Base64Encode(sha1data);
//     std::cout << "_64data: " << _64data << std::endl;
//     return 0;
// }

// 输出结果
// Original: Hello, World!
// Base64Encode: SGVsbG8sIFdvcmxkIQ==
// Base64Decode: Hello, World!
// AesEncrypted: 1112c2f4c5d53513bd6bd408ce45ae6d33e9f30da5d8fecab38e0a2e8de3148d
// AesDecrypted: Hello, World!
// Encrypt: 4r0YSBEC9+9BzYc4/9zM9Q==
// sha1:

// ▒*gr▒%W▒SU▒j▒B▒▒^
// sha1str: 0a0a9f2a6772942557ab5355d76af442f8f65e01
// _64data: CgqfKmdylCVXq1NV12r0Qvj2XgE=

