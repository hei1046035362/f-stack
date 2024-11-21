#include <rte_hash.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_hash_crc.h>

/// 主题：跨进程访问rte_hash 测试
/// 问题：hash函数是栈区，不同的进程，同一个函数的地址不一样，在其他进程访问会出错
/// 解决方案：在执行hash操作前，先计算hash值，避免使用内部调用hash操作的函数
/// create创建hash表并添加一个key，read程序读取这个key  两个都是secondary，如果没有master，可以把create改为master进程
/// g++ ./unittest/hash_create_test.cpp  -lstdc++ -lhiredis -I../ -L../ -lmt -lfstack -L${FF_PATH}/lib $(pkg-config --static --libs libdpdk) -lrt -lm -ldl -lcrypto -o hash_create
/// g++ ./unittest/hash_read_test.cpp  -lstdc++ -lhiredis -I../ -L../ -lmt -lfstack -L${FF_PATH}/lib $(pkg-config --static --libs libdpdk) -lrt -lm -ldl -lcrypto -o hash_read

struct rte_hash* init_hash(const char* hash_name, int ent_cnt, int key_len)
{
    struct rte_hash* _hash = rte_hash_find_existing(hash_name);
    if (_hash) {
        rte_hash_free(_hash);
        _hash = NULL;
    }

    struct rte_hash_parameters hash_params = {
        .name = hash_name,
        .entries = ent_cnt,
        .key_len = key_len,
        .hash_func = rte_hash_crc,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id(),
        .extra_flag = RTE_HASH_EXTRA_FLAGS_EXT_TABLE | 
                        RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD | 
                        RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF |
                        RTE_HASH_EXTRA_FLAGS_NO_FREE_ON_DEL, // 无锁并发+扩展桶
    };

    _hash = rte_hash_create(&hash_params);
    if (!_hash) {
        rte_exit(EXIT_FAILURE,
            "Failed to create hash table[%s]:%s:%d\n",
            hash_name, __func__, __LINE__);
    }
    RTE_LOG(INFO, USER1, "New hash created: %s\n",
        hash_name);
    return _hash;
}

int main(int argc, char* argv[]) {
    int ret;
    int i;
    char c_flag[] = "-c3";
    char n_flag[] = "-n4";
    char mp_flag[] = "--proc-type=secondary";
    char log_flag[] = "--log-level=6";
    char *argp[5];

    argp[0] = argv[0];
    argp[1] = c_flag;
    argp[2] = n_flag;
    argp[3] = mp_flag;
    argp[4] = log_flag;
    argc += 4;
    ret = rte_eal_init(argc, argp);
    const struct rte_hash* hash = init_hash("hello", 10, 20);
    if (hash != NULL) {
        printf("create hash hello OK.\n");
        rte_hash_add_key(hash, "hello");
    } else {
        printf("create hash hello Failed:%s.\n", rte_strerror(rte_errno));
    }


    ret = rte_eal_cleanup();
    if (ret)
        printf("Error from rte_eal_cleanup(), %d\n", ret);

    return 0;
}
