#ifndef __BTREE_INTERNAL_NODE_HPP__
#define __BTREE_INTERNAL_NODE_HPP__

#include "utils.hpp"
#include "btree/node.hpp"

// See internal_node_t in node.hpp

/* EPSILON used to prevent split then merge */
#define INTERNAL_EPSILON (sizeof(btree_key) + MAX_KEY_SIZE + sizeof(block_id_t))

//Note: This struct is stored directly on disk.  Changing it invalidates old data.
struct btree_internal_pair {
    block_id_t lnode;
    btree_key key;
} __attribute__((__packed__));


class internal_key_comp;

namespace internal_node_handler {
void init(block_size_t block_size, internal_node_t *node);
void init(block_size_t block_size, internal_node_t *node, const internal_node_t *lnode, const uint16_t *offsets, int numpairs);

block_id_t lookup(const internal_node_t *node, const btree_key *key);
bool insert(block_size_t block_size, internal_node_t *node, const btree_key *key, block_id_t lnode, block_id_t rnode);
bool remove(block_size_t block_size, internal_node_t *node, const btree_key *key);
void split(block_size_t block_size, internal_node_t *node, internal_node_t *rnode, btree_key *median);
void merge(block_size_t block_size, const internal_node_t *node, internal_node_t *rnode, btree_key *key_to_remove, internal_node_t *parent);
bool level(block_size_t block_size, internal_node_t *node, internal_node_t *rnode, btree_key *key_to_replace, btree_key *replacement_key, internal_node_t *parent);
int sibling(const internal_node_t *node, const btree_key *key, block_id_t *sib_id);
void update_key(internal_node_t *node, const btree_key *key_to_replace, const btree_key *replacement_key);
int nodecmp(const internal_node_t *node1, const internal_node_t *node2);
bool is_full(const internal_node_t *node);
bool is_underfull(block_size_t block_size, const internal_node_t *node);
bool change_unsafe(const internal_node_t *node);
bool is_mergable(block_size_t block_size, const internal_node_t *node, const internal_node_t *sibling, const internal_node_t *parent);
bool is_singleton(const internal_node_t *node);

void validate(block_size_t block_size, const internal_node_t *node);
void print(const internal_node_t *node);

size_t pair_size(const btree_internal_pair *pair);
const btree_internal_pair *get_pair(const internal_node_t *node, uint16_t offset);
btree_internal_pair *get_pair(internal_node_t *node, uint16_t offset);

// We can't use "internal" for internal stuff obviously.
namespace impl {
size_t pair_size_with_key(const btree_key *key);
size_t pair_size_with_key_size(uint8_t size);

void delete_pair(internal_node_t *node, uint16_t offset);
uint16_t insert_pair(internal_node_t *node, const btree_internal_pair *pair);
uint16_t insert_pair(internal_node_t *node, block_id_t lnode, const btree_key *key);
int get_offset_index(const internal_node_t *node, const btree_key *key);
void delete_offset(internal_node_t *node, int index);
void insert_offset(internal_node_t *node, uint16_t offset, int index);
void make_last_pair_special(internal_node_t *node);
bool is_equal(const btree_key *key1, const btree_key *key2);
}  // namespace impl
}  // namespace internal_node_handler

class internal_key_comp {
    const internal_node_t *node;
    const btree_key *key;
public:
    enum { faux_offset = 0 };

    explicit internal_key_comp(const internal_node_t *_node) : node(_node), key(NULL)  { }
    internal_key_comp(const internal_node_t *_node, const btree_key *_key) : node(_node), key(_key)  { }
    bool operator()(const uint16_t offset1, const uint16_t offset2) {
        const btree_key *key1 = offset1 == 0 ? key : &internal_node_handler::get_pair(node, offset1)->key;
        const btree_key *key2 = offset2 == 0 ? key : &internal_node_handler::get_pair(node, offset2)->key;
        return compare(key1, key2) < 0;
    }
    static int compare(const btree_key *key1, const btree_key *key2) {
        if (key1->size == 0 && key2->size == 0) //check for the special end pair
            return 0;
        else if (key1->size == 0)
            return 1;
        else if (key2->size == 0)
            return -1;
        else
            return sized_strcmp(key1->contents, key1->size, key2->contents, key2->size);
    }
};

#endif // __BTREE_INTERNAL_NODE_HPP__
