#include "rbtree.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "tgg_comm/tgg_common.h"
#include "log.hpp"
// extern struct rte_mempool* g_mempool_gid_rbnode = NULL;

// 创建红黑树
tgg_rbtree* tgg_rbtree_create(uint64_t fdidcid) {
    tgg_rbtree *tree = (tgg_rbtree*)dpdk_rte_malloc(__FILE__, __LINE__, sizeof(tgg_rbtree));
    if (!tree) return NULL;
    
    // 创建哨兵节点
    // if(high_freq_malloc(g_mempool_gid_rbnode, (void**)&tree->nil, sizeof(tgg_rb_node)) < 0) {
    //     free(tree);
    //     return NULL;
    // }
    tree->nil = (tgg_rb_node*)dpdk_rte_malloc(__FILE__, __LINE__, sizeof(tgg_rb_node));

    if (!tree->nil) {
        dpdk_rte_free(__FILE__, __LINE__, tree);
        return NULL;
    }
    
    // 初始化哨兵节点
    tree->nil->is_red = false;
    tree->nil->parent = tree->nil;
    tree->nil->left = tree->nil;
    tree->nil->right = tree->nil;
    
    // 根节点初始化为哨兵
    tree->root = tree->nil;
    
    return tree;
}

// 递归销毁所有节点
void destroy_nodes(tgg_rb_node *node, tgg_rb_node *nil) {
    if (node == nil) return;
    destroy_nodes(node->left, nil);
    destroy_nodes(node->right, nil);
    dpdk_rte_free(__FILE__, __LINE__, node);
}

// 销毁红黑树
void tgg_rbtree_destroy(tgg_rbtree *tree) {
    if (!tree) return;
    
    
    destroy_nodes(tree->root, tree->nil);
    dpdk_rte_free(__FILE__, __LINE__, tree->nil);
    dpdk_rte_free(__FILE__, __LINE__, tree);
}

// 左旋
static void left_rotate(tgg_rbtree *tree, tgg_rb_node *x) {
    tgg_rb_node *y = x->right;
    x->right = y->left;
    
    if (y->left != tree->nil) {
        y->left->parent = x;
    }
    
    y->parent = x->parent;
    
    if (x->parent == tree->nil) {
        tree->root = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    
    y->left = x;
    x->parent = y;
}

// 右旋
static void right_rotate(tgg_rbtree *tree, tgg_rb_node *y) {
    tgg_rb_node *x = y->left;
    y->left = x->right;
    
    if (x->right != tree->nil) {
        x->right->parent = y;
    }
    
    x->parent = y->parent;
    
    if (y->parent == tree->nil) {
        tree->root = x;
    } else if (y == y->parent->left) {
        y->parent->left = x;
    } else {
        y->parent->right = x;
    }
    
    x->right = y;
    y->parent = x;
}

// 插入修复
static void rb_insert_fixup(tgg_rbtree *tree, tgg_rb_node *z) {
    while (z->parent->is_red) {
        if (z->parent == z->parent->parent->left) {
            tgg_rb_node *y = z->parent->parent->right;
            
            if (y->is_red) {
                // Case 1: 叔叔节点是红色
                z->parent->is_red = false;
                y->is_red = false;
                z->parent->parent->is_red = true;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    // Case 2: z是右子节点
                    z = z->parent;
                    left_rotate(tree, z);
                }
                // Case 3: z是左子节点
                z->parent->is_red = false;
                z->parent->parent->is_red = true;
                right_rotate(tree, z->parent->parent);
            }
        } else {
            // 对称的情况
            tgg_rb_node *y = z->parent->parent->left;
            
            if (y->is_red) {
                z->parent->is_red = false;
                y->is_red = false;
                z->parent->parent->is_red = true;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    right_rotate(tree, z);
                }
                z->parent->is_red = false;
                z->parent->parent->is_red = true;
                left_rotate(tree, z->parent->parent);
            }
        }
    }
    tree->root->is_red = false;
}

// 插入节点
bool tgg_rbtree_insert(tgg_rbtree *tree, int64_t fdidcid) {
    if (!tree) return false;
    
    // 查找插入位置
    tgg_rb_node *y = tree->nil;
    tgg_rb_node *x = tree->root;
    
    while (x != tree->nil) {
        y = x;
        if (fdidcid < x->fdidcid) {
            x = x->left;
        } else if (fdidcid > x->fdidcid) {
            x = x->right;
        } else {
            // 节点已存在
            LOG_WARNING("Duplicate fdidcid[%lld] found.", fdidcid);
            return true;
        }
    }
    
    // 创建新节点
    tgg_rb_node *z = (tgg_rb_node*)dpdk_rte_malloc(__FILE__, __LINE__, sizeof(tgg_rb_node));
    // if(high_freq_malloc(g_mempool_gid_rbnode, (void**)&z, sizeof(tgg_rb_node)) < 0) {
    //     return false;
    // }
    if (!z) return false;
    
    z->fdidcid = fdidcid;
    z->parent = y;
    z->left = tree->nil;
    z->right = tree->nil;
    z->is_red = true;
    
    // 插入节点
    if (y == tree->nil) {
        tree->root = z;
    } else if (fdidcid < y->fdidcid) {
        y->left = z;
    } else {
        y->right = z;
    }
    
    // 修复红黑树性质
    rb_insert_fixup(tree, z);
    return true;
}

// 查找节点
tgg_rb_node* tgg_rbtree_find(tgg_rbtree *tree, int64_t fdidcid) {
    if (!tree) return NULL;
    
    tgg_rb_node *current = tree->root;
    while (current != tree->nil) {
        if (fdidcid < current->fdidcid) {
            current = current->left;
        } else if (fdidcid > current->fdidcid) {
            current = current->right;
        } else {
            return current;
        }
    }
    return NULL;
}

// 获取最小节点
tgg_rb_node* tgg_rbtree_minimum(tgg_rbtree *tree, tgg_rb_node *node) {
    if (!tree || node == tree->nil) return NULL;
    
    while (node->left != tree->nil) {
        node = node->left;
    }
    return node;
}

// 获取最大节点
tgg_rb_node* tgg_rbtree_maximum(tgg_rbtree *tree, tgg_rb_node *node) {
    if (!tree || node == tree->nil) return NULL;
    
    while (node->right != tree->nil) {
        node = node->right;
    }
    return node;
}

// 获取后继节点
tgg_rb_node* tgg_rbtree_successor(tgg_rbtree *tree, tgg_rb_node *node) {
    if (!tree || node == tree->nil) return NULL;
    
    if (node->right != tree->nil) {
        return tgg_rbtree_minimum(tree, node->right);
    }
    
    tgg_rb_node *y = node->parent;
    while (y != tree->nil && node == y->right) {
        node = y;
        y = y->parent;
    }
    return y;
}

// 获取前驱节点
tgg_rb_node* tgg_rbtree_predecessor(tgg_rbtree *tree, tgg_rb_node *node) {
    if (!tree || node == tree->nil) return NULL;
    
    if (node->left != tree->nil) {
        return tgg_rbtree_maximum(tree, node->left);
    }
    
    tgg_rb_node *y = node->parent;
    while (y != tree->nil && node == y->left) {
        node = y;
        y = y->parent;
    }
    return y;
}

// 移植节点
static void rb_transplant(tgg_rbtree *tree, tgg_rb_node *u, tgg_rb_node *v) {
    if (u->parent == tree->nil) {
        tree->root = v;
    } else if (u == u->parent->left) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }
    v->parent = u->parent;
}

// 删除修复
static void rb_delete_fixup(tgg_rbtree *tree, tgg_rb_node *x) {
    while (x != tree->root && !x->is_red) {
        if (x == x->parent->left) {
            tgg_rb_node *w = x->parent->right;
            
            if (w->is_red) {
                w->is_red = false;
                x->parent->is_red = true;
                left_rotate(tree, x->parent);
                w = x->parent->right;
            }
            
            if (!w->left->is_red && !w->right->is_red) {
                w->is_red = true;
                x = x->parent;
            } else {
                if (!w->right->is_red) {
                    w->left->is_red = false;
                    w->is_red = true;
                    right_rotate(tree, w);
                    w = x->parent->right;
                }
                
                w->is_red = x->parent->is_red;
                x->parent->is_red = false;
                w->right->is_red = false;
                left_rotate(tree, x->parent);
                x = tree->root;
            }
        } else {
            // 对称的情况
            tgg_rb_node *w = x->parent->left;
            
            if (w->is_red) {
                w->is_red = false;
                x->parent->is_red = true;
                right_rotate(tree, x->parent);
                w = x->parent->left;
            }
            
            if (!w->right->is_red && !w->left->is_red) {
                w->is_red = true;
                x = x->parent;
            } else {
                if (!w->left->is_red) {
                    w->right->is_red = false;
                    w->is_red = true;
                    left_rotate(tree, w);
                    w = x->parent->left;
                }
                
                w->is_red = x->parent->is_red;
                x->parent->is_red = false;
                w->left->is_red = false;
                right_rotate(tree, x->parent);
                x = tree->root;
            }
        }
    }
    x->is_red = false;
}

// 删除节点
bool tgg_rbtree_delete(tgg_rbtree *tree, int64_t fdidcid) {
    if (!tree) return false;
    
    // 查找要删除的节点
    tgg_rb_node *z = tgg_rbtree_find(tree, fdidcid);
    if (!z) return false;
    
    tgg_rb_node *y = z;
    tgg_rb_node *x = NULL;
    bool y_original_red = y->is_red;
    
    if (z->left == tree->nil) {
        x = z->right;
        rb_transplant(tree, z, z->right);
    } else if (z->right == tree->nil) {
        x = z->left;
        rb_transplant(tree, z, z->left);
    } else {
        y = tgg_rbtree_minimum(tree, z->right);
        y_original_red = y->is_red;
        x = y->right;
        
        if (y->parent == z) {
            x->parent = y;
        } else {
            rb_transplant(tree, y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        
        rb_transplant(tree, z, y);
        y->left = z->left;
        y->left->parent = y;
        y->is_red = z->is_red;
    }
    
    if (!y_original_red) {
        rb_delete_fixup(tree, x);
    }
    dpdk_rte_free(__FILE__, __LINE__, z);

    // free(z);
    return true;
}

// 中序遍历
void tgg_rbtree_inorder_walk(tgg_rbtree *tree, tgg_rb_node *node) {
    if (node == tree->nil) return;
    tgg_rbtree_inorder_walk(tree, node->left);
    printf("%ld ", node->fdidcid);
    tgg_rbtree_inorder_walk(tree, node->right);
}

void tgg_rbtree_getall_value(tgg_rbtree *tree, tgg_rb_node *node, std::vector<int64_t>& vec)
{
    if (node == tree->nil) return;
    tgg_rbtree_getall_value(tree, node->left, vec);
    vec.push_back(node->fdidcid);
    tgg_rbtree_getall_value(tree, node->right, vec);
}

int tgg_rbtree_getall_value(tgg_rbtree *tree, tgg_rb_node *node, tgg_fd_list** lst_head)
{
    if (node == tree->nil || node == NULL) {
        return 0;
    }
    
    // 递归遍历左子树
    if (node->left != tree->nil && node->left != NULL) {
        if (tgg_rbtree_getall_value(tree, node->left, lst_head) != 0) {
            return -1;
        }
    }
    
    // 创建新节点
    tgg_fd_list* new_node = (tgg_fd_list*)dpdk_rte_malloc(__FILE__, __LINE__, sizeof(tgg_fd_list));
    if (!new_node) {
        LOG_ERROR("malloc for get rbtree value failed.");
        return -1;
    }
    new_node->fdidcid = node->fdidcid;
    new_node->next = NULL;
    
    // 将新节点添加到链表尾部（避免逆序）
    if (*lst_head == NULL) {
        *lst_head = new_node;
    } else {
        // tgg_fd_list* current = *lst_head;
        // while (current->next != NULL) {
        //     current = current->next;
        // }
        // current->next = new_node;

        new_node->next = *lst_head;
        *lst_head = new_node;
    }
    
    // 递归遍历右子树
    if (node->right != tree->nil && node->right != NULL) {
        if (tgg_rbtree_getall_value(tree, node->right, lst_head) != 0) {
            return -1;
        }
    }
    
    return 0;
}

// 获取节点数量
static int rb_size(tgg_rb_node *node, tgg_rb_node *nil) {
    if (node == nil) return 0;
    return 1 + rb_size(node->left, nil) + rb_size(node->right, nil);
}

int tgg_rbtree_size(tgg_rbtree *tree) {
    if (!tree) return 0;
    return rb_size(tree->root, tree->nil);
}

// 检查红黑树性质
static bool rb_validate(tgg_rb_node *node, tgg_rb_node *nil, int black_count, int* path_black_count) {
    if (node == nil) {
        if (*path_black_count == -1) {
            *path_black_count = black_count;
        }
        return (*path_black_count == black_count);
    }
    
    // 检查红色节点的子节点不能是红色
    if (node->is_red) {
        if (node->left->is_red || node->right->is_red) {
            return false;
        }
    }
    
    // 递归检查左右子树
    int new_black_count = black_count + (node->is_red ? 0 : 1);
    return rb_validate(node->left, nil, new_black_count, path_black_count) &&
           rb_validate(node->right, nil, new_black_count, path_black_count);
}

bool tgg_rbtree_validate(tgg_rbtree *tree) {
    if (!tree || !tree->root || tree->root == tree->nil) return true;
    
    // 根节点必须是黑色
    if (tree->root->is_red) return false;
    
    int path_black_count = -1;
    return rb_validate(tree->root, tree->nil, 0, &path_black_count);
}