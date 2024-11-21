#include <rte_hash.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_debug.h>
#include <rte_hash_crc.h>



struct rte_hash* init_hash(const char* hash_name, int ent_cnt, int key_len)
{
    const char* key = "hello";
    void* data;
    struct rte_hash* _hash = rte_hash_find_existing(hash_name);
    if (_hash) {
        if (rte_hash_lookup_with_hash_data(_hash, key, rte_hash_crc(key, strlen(key), 0), &data) == 0) {
            printf("key:%s\n", key);
        }
    }

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

    if (init_hash("hello", 10, 20) != NULL) {
        printf("find hash hello OK.\n");
    } else {
        printf("create hash hello Failed:%s.\n", rte_strerror(rte_errno));
    }


    ret = rte_eal_cleanup();
    if (ret)
        printf("Error from rte_eal_cleanup(), %d\n", ret);

    return 0;
}
