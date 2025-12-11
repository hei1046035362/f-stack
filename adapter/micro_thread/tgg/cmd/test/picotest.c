#include "picohttpparser.h"
#include "stdio.h"
#include "string.h"

int parse_query_string(const char* query_str, size_t query_len, 
                      struct phr_header* params, size_t max_params) {
    if (query_len == 0) {
        return 0;
    }
    
    int param_count = 0;
    const char* ptr = query_str;
    const char* end = query_str + query_len;
    
    while (ptr < end && param_count < (int)max_params) {
        // 跳过开头的'&'
        if (*ptr == '&') {
            ptr++;
            continue;
        }
        
        // 记录key的起始位置
        const char* key_start = ptr;
        
        // 查找key的结束位置（遇到=或&或字符串结束）
        while (ptr < end && *ptr != '=' && *ptr != '&') {
            ptr++;
        }
        
        size_t key_len = ptr - key_start;
        
        // 处理value
        const char* value_start = NULL;
        size_t value_len = 0;
        
        if (ptr < end && *ptr == '=') {
            ptr++; // 跳过'='
            value_start = ptr;
            
            // 查找value的结束位置（遇到&或字符串结束）
            while (ptr < end && *ptr != '&') {
                ptr++;
            }
            
            value_len = ptr - value_start;
        } else {
            // 只有key没有value的情况
            value_start = key_start + key_len; // 指向key结束的位置
            value_len = 0;
        }
        
        // 存储参数
        if (key_len > 0) {
            params[param_count].name = key_start;
            params[param_count].name_len = key_len;
            params[param_count].value = value_start;
            params[param_count].value_len = value_len;
            param_count++;
        }
        
        // 跳过'&'，准备处理下一个参数
        if (ptr < end && *ptr == '&') {
            ptr++;
        }
    }
    
    return param_count;
}

int parse_http_request(const char* buf, int len) {
    const char* method;
    size_t method_len;
    const char* path;
    size_t path_len;
    int minor_version;
    struct phr_header headers[100];
    size_t num_headers = 100;
    int ret;

    // 解析请求行和头部
    ret = phr_parse_request(buf, len, &method, &method_len, &path, &path_len,
                           &minor_version, headers, &num_headers, 0);

    if (ret > 0) {
        // 成功，ret是请求的长度（包括头部结束）
        printf("Method: %.*s\n", (int)method_len, method);
        printf("Path: %.*s\n", (int)path_len, path);

        for (size_t i = 0; i < num_headers; i++) {
            printf("%.*s: %.*s\n",
                   (int)headers[i].name_len, headers[i].name,
                   (int)headers[i].value_len, headers[i].value);
            if(headers[i].name_len == 4 && 0 == strncmp(headers[i].name, "Path", 4)) {
                printf("param\n");
                struct phr_header param[100];
                size_t num_params = 100;
                ret = phr_parse_headers(headers[i].value+2, headers[i].value_len-2, param, &num_params, 0);
                for (size_t j = 0; j < num_params; j++) {
                    printf("%.*s: %.*s\n",
                           (int)param[j].name_len, param[j].name,
                           (int)param[j].value_len, param[j].value);
                }

            }
        }
        struct phr_header param[100];
        size_t num_params = 100;
        ret = parse_query_string(path, path_len, param, &num_params);
        printf("print params:%d, ret:%d\n", num_params, ret);
        for (size_t j = 0; j < ret; j++) {
            printf("%.*s: %.*s\n",
                   (int)param[j].name_len, param[j].name,
                   (int)param[j].value_len, param[j].value);
        }

    }
    return ret;
}
int main(int argc, char* argv[]) {
    const char* req = "GET /?locale=zh-CN&client_properties=eyJvcyI6ImlvcyIsInZlcnNpb24iOiIxLjAuMCIsImJ1aWxkX251bWJlciI6Ijc2MyIsImRldmljZV9pZCI6IjVEMTc1NEUxLTAzNUMtNDQ1My1BOEJDLUJBN0Q1ODdGNTQ4NyJ9&authorization=a58b6a4695401b1c1522c322dedac4730dc3ab73c9247155e412defb6be6b2ad56f5c117848523f46ecaf7f18dd99c340286425160a77616bf0b1fa3b42cd1235a3e5afc9671f625a5948f4c7c720d89b7f9a60ac83e9d8d6c4f677b5a0ae420a199ea76b55d1a89f829a3bf9328df50&token=a58b6a4695401b1c1522c322dedac4730dc3ab73c9247155e412defb6be6b2ad56f5c117848523f46ecaf7f18dd99c340286425160a77616bf0b1fa3b42cd1235a3e5afc9671f625a5948f4c7c720d89b7f9a60ac83e9d8d6c4f677b5a0ae420a199ea76b55d1a89f829a3bf9328df50 HTTP/1.1\r\nHost: 192.168.40.129:8058\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: t3Wu9fK4S02wWvhhnD0CEQ==\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\nUser-Agent: Python/3.10 websockets/13.1\r\n\r\n";
    parse_http_request(req, strlen(req));
    return 0;
}
