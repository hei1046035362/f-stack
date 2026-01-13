#ifndef TGG_RBTREE_H
#define TGG_RBTREE_H

#include <stdint.h>
#include <stdbool.h>
#include <vector>

// 红黑树节点结构
typedef struct st_tgg_rb_node {
    int64_t fdidcid;           // 键值
    struct st_tgg_rb_node *parent;  // 父节点
    struct st_tgg_rb_node *left;    // 左子节点
    struct st_tgg_rb_node *right;   // 右子节点
    bool is_red;               // 颜色标记，true为红色
} tgg_rb_node;

// 红黑树结构
typedef struct st_tgg_rbtree {
    tgg_rb_node *root;         // 根节点
    tgg_rb_node *nil;          // 哨兵节点
} tgg_rbtree;

// 初始化红黑树
tgg_rbtree* tgg_rbtree_create(void);

// 销毁红黑树
void tgg_rbtree_destroy(tgg_rbtree *tree);

// 插入节点
bool tgg_rbtree_insert(tgg_rbtree *tree, int64_t fdidcid);

// 删除节点
bool tgg_rbtree_delete(tgg_rbtree *tree, int64_t fdidcid);

// 查找节点
tgg_rb_node* tgg_rbtree_find(tgg_rbtree *tree, int64_t fdidcid);

// 获取最小节点
tgg_rb_node* tgg_rbtree_minimum(tgg_rbtree *tree, tgg_rb_node *node);

// 获取最大节点
tgg_rb_node* tgg_rbtree_maximum(tgg_rbtree *tree, tgg_rb_node *node);

// 获取后继节点
tgg_rb_node* tgg_rbtree_successor(tgg_rbtree *tree, tgg_rb_node *node);

// 获取前驱节点
tgg_rb_node* tgg_rbtree_predecessor(tgg_rbtree *tree, tgg_rb_node *node);

// 中序遍历（用于调试）
void tgg_rbtree_inorder_walk(tgg_rbtree *tree, tgg_rb_node *node);

void tgg_rbtree_getall_value(tgg_rbtree *tree, tgg_rb_node *node, std::vector<int64_t>& vec);

// 获取节点数量
int tgg_rbtree_size(tgg_rbtree *tree);

// 检查红黑树性质（用于调试）
bool tgg_rbtree_validate(tgg_rbtree *tree);

#endif // TGG_RBTREE_H