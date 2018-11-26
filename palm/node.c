/**
 *    author:     UncP
 *    date:    2018-08-19
 *    license:    BSD-3
**/

#include <stdlib.h>
#include <string.h>
#include <assert.h>
// TODO: remove this
#include <stdio.h>

#include "allocator.h"
#include "node.h"

static uint32_t node_size  = node_min_size;
static uint32_t batch_size = node_min_size;
static uint32_t node_offset = 0;
static uint32_t node_id = 0;

#define node_size_mask (~0xfff)

void set_node_size(uint32_t size)
{
  node_size = size < node_min_size ? node_min_size : size > node_max_size ? node_max_size : size;
  node_size &= node_size_mask;
}

uint32_t get_node_size()
{
  return node_size;
}

void set_batch_size(uint32_t size)
{
  batch_size = size < node_min_size ? node_min_size : size > node_max_size ? node_max_size : size;
  batch_size &= node_size_mask;
}

uint32_t get_batch_size()
{
  return batch_size;
}

// for blink node, `node_size` is not really node size
void set_node_offset(uint32_t offset)
{
  node_offset = offset;
}

// get the ptr to the key length byte
#define get_ptr(n, off) ((char *)n->data + off)
// get the length of the key
#define get_len(n, off) ((uint32_t)(*(len_t *)get_ptr(n, off)))
#define get_key(n, off) (get_ptr(n, off) + key_byte)
#define get_val(n, off) ((void *)(*(val_t *)(get_key(n, off) + get_len(n, off))))
#define node_index(n)   ((index_t *)((char *)n + (node_size - node_offset - (n->keys * index_byte))))
#define get_key_info(n, off, key, len) \
  const void *key = get_key(n, off);   \
  uint32_t len = get_len(n, off);
#define get_kv_info(n, off, key, len, val) \
  get_key_info(n, off, key, len);          \
  void *val = get_val(n, off);
#define get_kv_ptr(n, off, key, len, val)                 \
  const void *key = get_key(n, off);                      \
  uint32_t len = get_len(n, off);                         \
  void *val = (void *)((char *)key + len);

int compare_key(const void *key1, uint32_t len1, const void *key2, uint32_t len2)
{
  uint32_t min = len1 < len2 ? len1 : len2;
  int r = memcmp(key1, key2, min);
  return r ? r : (len1 == len2 ? 0 : (len1 < len2 ? -1 : +1));
}

/****** NODE operation ******/

node* new_node(uint8_t type, uint8_t level)
{
  uint32_t size = likely(type < Batch) ? node_size : batch_size;
#ifdef Allocator
  node *n = (node *)allocator_alloc(size);
#else
  node *n = (node *)malloc(size);
#endif

  node_init(n, type, level);

  return n;
}

inline void node_init(node *n, uint8_t type, uint8_t level)
{
  n->type  = type;
  n->level = level;
  n->pre   = 0;
  n->sopt  = 0;
  // multiple threads can be creating new node at the same time, increase node-id atomically
  n->id    = __atomic_fetch_add(&node_id, 1, __ATOMIC_RELAXED);
  n->keys  = 0;
  n->off   = 0;
  n->next  = 0;
  n->first = 0;
}

void free_node(node *n)
{
  #ifdef Allocator
    allocator_free((void *)n);
  #else
    free((void *)n);
  #endif
}

void node_prefetch(node *n)
{
  // 0 means for read,
  // 3 means node has a high degree of temporal locality to stay in all levels of cache if possible
  // TODO: change locality according to level?
  __builtin_prefetch(n, 0 /* rw */, 3 /* locality */);
}

// DFS free each node
void free_btree_node(node *n)
{
#ifdef Allocator
  (void)n;
#else
  if (n == 0) return ;

  if (n->level) {
    free_btree_node(n->first);
    assert(n->keys);
    index_t *index = node_index(n);
    for (uint32_t i = 0; i < n->keys; ++i) {
      node *child = (node *)get_val(n, index[i]);
      free_btree_node(child);
    }
  }

  free_node(n);
#endif
}

// whether we should insert key into this node, if not, return (their common prefix length) + 1
// this function is only called after `node_insert` is called
int node_is_before_key(node *n, const void *key, uint32_t len)
{
  assert(n->level == 0 || (n->type & Blink));

  if (n->pre) {
    assert(n->level == 0);
    // TODO: remove this if we can handle key length <= prefix length
    assert(len > n->pre);
    char *lptr = (char *)n->data, *rptr = (char *)key;
    for (uint32_t i = 0; i < n->pre; ++i)
      if (lptr[i] != rptr[i])
        return i + 1;
  }

  const void *key1 = (char *)key + n->pre;
  uint32_t    len1 = len - n->pre;

  index_t *index = node_index(n);
  assert(n->keys);
  get_key_info(n, index[n->keys - 1], key2, len2);
  // equal is not possible
  if (compare_key(key2, len2, key1, len1) > 0)
    return 0;

  char *lptr = (char *)key2, *rptr = (char *)key1;
  uint32_t i = 0;
  for (; i < len2 && i < len1; ++i)
    if (lptr[i] != rptr[i])
      return i + 1 + n->pre;

  uint32_t r = i + n->pre;
  assert(r <= len);
  return r;
}

node* node_descend(node *n, const void *key, uint32_t len)
{
  assert(n->level && n->keys && !n->pre);
  index_t *index = node_index(n);

  int first = 0, count = (int)n->keys;

  while (count > 0) {
    int half = count >> 1;
    int middle = first + half;

    get_key_info(n, index[middle], key1, len1);

    if (compare_key(key1, len1, key, len) <= 0) {
      first = middle + 1;
      count -= half + 1;
    } else {
      count = half;
    }
  }
  return likely(first) ? (node *)get_val(n, index[first - 1]) : n->first;
}

// find the key in the leaf, return its pointer, if no such key, return 0
// if this is a blink node and we need to move right, return -1
void* node_search(node *n, const void *key, uint32_t len)
{
  assert(n->level == 0);

  if (n->pre) {
    // TODO: remove this if we can handle key length <= prefix length
    assert(len > n->pre);
    if (compare_key(n->data, n->pre, key, n->pre)) // compare with node prefix
      return 0;
  }

  const void *key1 = (char *)key + n->pre;
  uint32_t    len1 = len - n->pre;

  int low = 0, high = (int)n->keys - 1;
  index_t *index = node_index(n);
  while (low <= high) {
    int mid = (low + high) / 2;

    get_key_info(n, index[mid], key2, len2);

    int r = compare_key(key2, len2, key1, len1);
    if (r == 0) {
      return get_val(n, index[mid]);
    } else if (r < 0) {
      low  = mid + 1;
    } else {
      high = mid - 1;
    }
  }

  if (unlikely(low == (int)n->keys) && low && (n->type & Blink))
    return (void *)-1;

  return (void *)0;
}

// if we can do a prefix compression and fit the new key in this node, return 1; else return 0
// note: this is a little bit time consuming
static int node_try_prefix_compression(node *n, const void *key, uint32_t len)
{
#ifdef Prefix
  // prefix compression is only supported in level 0
  if (n->level)
    return 0;

  if (--len == 0) return 0;
  char *lptr = (char *)key;
  uint32_t prelen = len;

  // see if we can get new prefix for all the key
  assert(n->keys);
  index_t *index = node_index(n);
  for (uint32_t i = 0; i < n->keys; ++i) {
    get_key_info(n, index[i], rkey, rlen);
    assert(rlen);
    if (--rlen == 0) return 0;
    char *rptr = (char *)rkey;
    uint32_t curlen = 0;
    for (uint32_t j = 0; j < len && j < rlen; ++j) {
      if (lptr[j] != rptr[j])
        break;
      ++curlen;
    }
    if (curlen == 0) return 0;
    prelen = prelen < curlen ? prelen : curlen;
  }

  assert(prelen);

  // now calculate whether we can fit the new key in this node after prefix compression
  uint32_t new_off = n->off - prelen * n->keys + key_byte + len + 1 + value_bytes + index_byte;
  if ((n->data + new_off) > (char *)index)
    return 0;

  // copy new node content to `buf`, adjust node index at the same time
  char buf[node_size];
  uint32_t off = 0;
  memcpy(buf, n->data, n->pre);
  off += n->pre;
  memcpy(buf + off, key, prelen);
  off += prelen;
  for (uint32_t i = 0; i < n->keys; ++i) {
    get_key_info(n, index[i], k, l);
    index[i] = off;
    uint32_t nl = l - prelen;
    *((len_t *)(buf + off)) = (len_t)nl;
    off += key_byte;
    memcpy(buf + off, k + prelen, nl + value_bytes);
    off += nl + value_bytes;
  }

  // copy new node content back
  memcpy(n->data, buf, off);
  // assign new offset
  n->off = off;
  // assign new prefix length
  n->pre += prelen;
  return 1;
#else
  (void)n;
  (void)key;
  (void)len;
  return 0;
#endif
}

static void node_insert_kv(node *n, const void *key, uint32_t len, const void *val)
{
  *((len_t *)(n->data + n->off)) = (len_t)len;
  n->off += key_byte;
  memcpy(n->data + n->off, key, len);
  n->off += len;
  if (likely(val))
    *((val_t *)(n->data + n->off)) = *(val_t *)(&val);
  else
    *((val_t *)(n->data + n->off)) = 0;
  n->off += value_bytes;

  ++n->keys;
}

// insert a kv into node:
//   if key already exists, return 0
//   if there is prefix conflict, or not enough space, return -1
//   if there is not enough space, return -1
//   if this is a blink node and we need to move right, return -2
//   if succeed, return 1
int node_insert(node *n, const void *key, uint32_t len, const void *val)
{
  if (n->pre) { // compare with node prefix
    assert(n->level == 0);
    // TODO: remove this if we can handle key length <= prefix length
    assert(len > n->pre);
    if (compare_key(n->data, n->pre, key, n->pre))
      return -1;
  }

  void *key1 = (char *)key + n->pre;
  uint32_t len1 = len - n->pre;

  // find the index which to insert the key
  int low = 0, high = (int)n->keys - 1;
  index_t *index = node_index(n);
  while (low <= high) {
    int mid = (low + high) / 2;

    get_key_info(n, index[mid], key2, len2);

    int r = compare_key(key2, len2, key1, len1);
    if (r == 0)
      return 0;
    else if (r < 0)
      low  = mid + 1;
    else
      high = mid - 1;
  }

  // key does not exist, we can proceed

  if (unlikely((low == (int)n->keys) && low && (n->type & Blink)))
    return -2;

  // check if there is enough space
  if (unlikely((n->data + (n->off + key_byte + len1 + value_bytes + index_byte)) > (char *)index)) {
    if (!node_try_prefix_compression(n, key1, len1)) return -1;
    // need to update new key suffix
    key1 = (char *)key + n->pre;
    len1 = len - n->pre;
    assert((n->data + (n->off + key_byte + len1 + value_bytes + index_byte)) <= (char *)index);
  }

  // update index
  --index;
  if (likely(low)) memmove(&index[0], &index[1], low * index_byte);
  index[low] = n->off;

  node_insert_kv(n, key1, len1, val);

  return 1;
}

// split half of the node entries from `old` to `new`
void node_split(node *old, node *new, char *pkey, uint32_t *plen)
{
  uint32_t left = old->keys / 2, right = old->keys - left;
  index_t *l_idx = node_index(old), *r_idx = node_index(new);
  *plen = 0;

  if (old->pre) { // copy prefix
    assert(old->level == 0);
    memcpy(new->data, old->data, old->pre);
    new->pre = old->pre;
    new->off = new->pre;
    memcpy(pkey, old->data, old->pre);
    *plen = old->pre;
  }

  get_kv_info(old, l_idx[left], fkey, flen, fval);
  // if we are at level 0 and this is not a blink node, get prefix key
  if (likely(old->level == 0) && (old->type & Blink) == 0) {
    // Reference: Prefix B-Trees
    assert(left);
    get_key_info(old, l_idx[left - 1], lkey, llen);
    char *lptr = (char *)lkey, *rptr = (char *)fkey;
    for (uint32_t i = 0; i < llen && i < flen; ++i) {
      pkey[(*plen)++] = rptr[i];
      if (lptr[i] != rptr[i])
        break;
    }
  } else {
    memcpy(pkey + *plen, fkey, flen);
    *plen += flen;
  }

  if (unlikely(old->level)) { // assign first child if it's not a level 0 node
    new->first = fval;
    --right;        // one key will be promoted to upper level
    flen += key_byte + value_bytes;
  } else {
    flen = 0;
  }

  // we first copy all the keys to `new` in sequential order,
  // then move the first half back to `old` and adjust the other half in `new`
  // the loop has some optimization, it does not have good readability
  uint32_t length = 0;
  r_idx -= old->keys;
  for (uint32_t i = 0, j = old->keys - right; i < old->keys; ++i) {
    r_idx[i] = new->off;
    get_kv_info(old, l_idx[i], okey, olen, oval);
    node_insert_kv(new, okey, olen, oval);
    if (i == left - 1)
      length = new->off - new->pre;
    if (i >= j)
      r_idx[i] -= length + flen;
  }

  // copy the first half data, including prefix
  memcpy(old->data + old->pre, new->data + new->pre, length);
  old->keys = left;
  old->off  = length + old->pre;
  // update `old` index
  l_idx = node_index(old);
  r_idx = node_index(new);
  memcpy(l_idx, r_idx, left * index_byte);

  // update `length` with fence key length
  length += flen;
  // adjust `new` layout
  new->keys = right;
  new->off -= length;
  memmove(new->data + new->pre, new->data + new->pre + length, new->off - new->pre);

  // update node link
  new->next = old->next;
  old->next = new;
}

// for blink node, insert the first key of `new` as fence key for `old`
void node_insert_fence(node *old, node *new, void *next, char *pkey, uint32_t *plen)
{
  // remove `blink` type to help insert fence key
  old->type &= (~(uint8_t)Blink);

  if (likely(old->level == 0)) {
    index_t *nidx = node_index(new);
    assert(new->keys);
    get_key_info(new, nidx[0], key, len);
    memcpy(pkey, key, len);
    *plen = len;
    assert(node_insert(old, key, len, next) == 1);
  } else {
    assert(node_insert(old, pkey, *plen, next) == 1);
  }

  // restore `blink` type
  old->type |= Blink;

  // overwrite the old node's `next` field
  old->next = (node *)next;
}

/****** BATCH operation ******/

#define get_op(n, off) ((uint32_t)(*(uint8_t *)(get_ptr(n, off) - sizeof(uint8_t))))

batch* new_batch()
{
  return new_node(Batch, 0);
}

void free_batch(batch *b)
{
  free_node((node *)b);
}

void batch_clear(batch *b)
{
  b->keys = 0;
  b->off  = 0;
}

// insert a kv into node, this function allows duplicate key
static int batch_write(batch *b, uint32_t op, const void *key1, uint32_t len1, const void *val)
{
  int low = 0, high = (int)b->keys - 1;
  index_t *index = node_index(b);

  while (low <= high) {
    int mid = (low + high) / 2;

    get_key_info(b, index[mid], key2, len2);

    int r = compare_key(key2, len2, key1, len1);
    if (r <= 0)
      low  = mid + 1;
    else
      high = mid - 1;
  }

  --index;

  // check if there is enough space
  if (unlikely((char *)b->data + (b->off + sizeof(uint8_t) /* op */ + key_byte + len1 + value_bytes) > (char *)index))
    return -1;

  // set op type before kv
  *((uint8_t *)(b->data + b->off)) = (uint8_t)op;
  b->off += sizeof(uint8_t);

  if (likely(low)) memmove(&index[0], &index[1], low * index_byte);
  index[low] = b->off;

  node_insert_kv(b, key1, len1, val);

  return 1;
}

int batch_add_write(batch *b, const void *key, uint32_t len, const void *val)
{
  if (key == 0 || len == 0) return 1;
  return batch_write(b, Write, key, len, val);
}

int batch_add_read(batch *b, const void *key, uint32_t len)
{
  if (key == 0 || len == 0) return 1;
  return batch_write(b, Read, key, len, 0);
}

// read a kv at index
void batch_read_at(batch *b, uint32_t idx, uint32_t *op, void **key, uint32_t *len, void **val)
{
  // TODO: remove this
  assert(idx < b->keys);
  index_t *index = node_index(b);
  get_kv_ptr(b, index[idx], k, l, v);
  *op = get_op(b, index[idx]);
  *key = (void *)k;
  *len = l;
  *val = (void *)v;
}

void* batch_get_value_at(batch *b, uint32_t idx)
{
  if (idx >= b->keys || b->keys == 0) return 0;
  index_t *index = node_index(b);
  return get_val(b, index[idx]);
}

void path_clear(path *p)
{
  p->depth = 0;
}

void path_copy(const path *src, path *dst)
{
  dst->depth = src->depth;
  memcpy(dst->nodes, src->nodes, src->depth * sizeof(node *));
}

void path_set_kv_id(path *p, uint32_t id)
{
  p->id = id;
}

uint32_t path_get_kv_id(path *p)
{
  return p->id;
}

void path_push_node(path *p, node *n)
{
  // TODO: remove this
  assert(p->depth < max_descend_depth);
  p->nodes[p->depth++] = n;
}

node* path_get_node_at_level(path *p, uint32_t level)
{
  // TODO: remove this
  assert(p->depth > level);
  return p->nodes[p->depth - level - 1];
}

node* path_get_node_at_index(path *p, uint32_t idx)
{
  // TODO: remove this
  assert(p->depth > idx);
  return p->nodes[idx];
}

#ifdef Test

#include <stdio.h>

static char* format_kv(char *ptr, char *end, node* n, uint32_t off)
{
  ptr += snprintf(ptr, end - ptr, "%4u  ", off);
  uint32_t len = get_len(n, off);
  ptr += snprintf(ptr, end - ptr, "%u  ", len);
  snprintf(ptr, len + 1, "%s", get_key(n, off));
  ptr += len;
  ptr += snprintf(ptr, end - ptr, "  %llu\n", (val_t)get_val(n, off));
  return ptr;
}

static char* format_child(char *ptr, char *end, node* n, uint32_t off)
{
  // ptr += snprintf(ptr, end - ptr, "%4u  ", off);
  uint32_t len = get_len(n, off);
  ptr += snprintf(ptr, end - ptr, "%u  ", len);
  snprintf(ptr, len + 1, "%s", get_key(n, off));
  ptr += len;
  if (!(n->type & Blink)) {
    node *child = (node *)get_val(n, off);
    ptr += snprintf(ptr, end - ptr, "  %u\n", child->id);
  } else {
    ptr += snprintf(ptr, end - ptr, "\n");
  }
  return ptr;
}

void node_print(node *n, int detail)
{
  assert(n);
  int size = node_size * 2;
  char buf[size], *ptr = buf, *end = buf + size;
  char* (*format)(char *, char *, node *, uint32_t) = n->level == 0 ? format_kv : format_child;

  ptr += snprintf(ptr, end - ptr, "id: %u  ", n->id);
  ptr += snprintf(ptr, end - ptr, "type: %s  ",
    (n->type & Root) ? "root" : (n->type & Branch) ? "branch" : "leaf");
  ptr += snprintf(ptr, end - ptr, "level: %u  ", n->level);
  ptr += snprintf(ptr, end - ptr, "keys: %u  ", n->keys);
  snprintf(ptr, n->pre + 9, "prefix: %s", n->data);
  ptr += n->pre + 8;
  ptr += snprintf(ptr, end - ptr, "  offset: %u\n", n->off);

  if (n->level && (n->type & Blink) == 0)
    ptr += snprintf(ptr, end - ptr, "first: %u\n", n->first->id);

  index_t *index = node_index(n);
  if (detail) {
    for (uint32_t i = 0; i < n->keys; ++i)
      ptr = (*format)(ptr, end, n, index[i]);
  } else {
    if (n->keys > 0)
      ptr = (*format)(ptr, end, n, index[0]);
    if (n->keys > 1)
      ptr = (*format)(ptr, end, n, index[n->keys - 1]);
  }

  if (n->next && (n->type & Blink) == 0)
    ptr += snprintf(ptr, end - ptr, "next: %u\n", n->next->id);

  printf("%s\n", buf);
}

void batch_print(batch *b, int detail)
{
  assert(b);
  int size = (float)node_size * 1.5;
  char buf[size], *ptr = buf, *end = buf + size;

  ptr += snprintf(ptr, end - ptr, "keys: %u  ", b->keys);
  ptr += snprintf(ptr, end - ptr, "  offset: %u\n", b->off);

  index_t *index = node_index(b);
  if (detail) {
    for (uint32_t i = 0; i < b->keys; ++i) {
      ptr += snprintf(ptr, end - ptr, "%s ", get_op(b, index[i]) == Write ? "w" : "r");
      ptr = format_kv(ptr, end, b, index[i]);
    }
  } else {
    if (b->keys > 0) {
      ptr += snprintf(ptr, end - ptr, "%s ", get_op(b, index[0]) == Write ? "w" : "r");
      ptr = format_kv(ptr, end, b, index[0]);
    }
    if (b->keys > 1) {
      ptr += snprintf(ptr, end - ptr, "%s ", get_op(b, index[b->keys - 1]) == Write ? "w" : "r");
      ptr = format_kv(ptr, end, b, index[b->keys - 1]);
    }
  }

  printf("%s\n", buf);
}

// verify that all keys in node are in ascending order
static void validate(node *n, int is_batch)
{
  assert(n);

  if (n->keys == 0) return ;

  index_t *index = node_index(n);
  char *pre_key = get_key(n, index[0]);
  uint32_t pre_len = get_len(n, index[0]);

  for (uint32_t i = 1; i < n->keys; ++i) {
    char *cur_key = get_key(n, index[i]);
    uint32_t cur_len = get_len(n, index[i]);
    if (is_batch == 0)
      assert(compare_key(pre_key, pre_len, cur_key, cur_len) < 0);
    else
      assert(compare_key(pre_key, pre_len, cur_key, cur_len) <= 0);
    pre_key = cur_key;
    pre_len = cur_len;
  }
}

void node_validate(node *n)
{
  validate(n, 0);
}

void batch_validate(batch *n)
{
  validate(n, 1);
}

void node_get_whole_key(node *n, uint32_t idx, char *key, uint32_t *len)
{
  assert(idx <= n->keys);
  index_t *index = node_index(n);
  get_key_info(n, index[idx], buf, buf_len);
  if (n->pre) {
    assert(n->level == 0);
    memcpy(key, n->data, n->pre);
  }
  memcpy(key + n->pre, buf, buf_len);
  *len = buf_len + n->pre;
}

// this function is used to verify that a b+tree node is validate
// in the tree, it verifies 4 aspects:
//   1. the key in this node is in ascending order
//   2. the last key in this node is smaller than the first key in next node
//   3. the first key in this node is larger than the last key in the first child
//   4. the last key in this node is smaller than or equal the first key in the last child
void btree_node_validate(node *n)
{
  if (n == 0) return ;

  node_validate(n);

  if (n->keys == 0) return ; // tree is empty

  char first_key[max_key_size], last_key[max_key_size];
  uint32_t first_len, last_len;
  node_get_whole_key(n, 0, first_key, &first_len);
  node_get_whole_key(n, n->keys - 1, last_key, &last_len);

  // validate the last key in this node is smaller than the first key in next node
  if (n->next) {
    char next_key[max_key_size];
    uint32_t next_len;
    node_get_whole_key(n->next, 0, next_key, &next_len);
    assert(compare_key(last_key, last_len, next_key, next_len) < 0);
  }

  if (n->level) {
    assert(n->first != 0);

    // validate that the first key in this node is larger than the last key in the first child
    char child_last_key[max_key_size], child_first_key[max_key_size];
    uint32_t child_last_len, child_first_len;
    node_get_whole_key(n->first, n->first->keys - 1, child_last_key, &child_last_len);
    assert(compare_key(child_last_key, child_last_len, first_key, first_len) < 0);

    // validate that the last key in this node is smaller than or equal the first key in the last child
    index_t *index = node_index(n);
    node *last_child = (node *)get_val(n, index[n->keys - 1]);
    node_get_whole_key(last_child, 0, child_first_key, &child_first_len);
    assert(compare_key(last_key, last_len, child_first_key, child_first_len) <= 0); // equal is valid

  } else {
    assert(n->first == 0);
  }
}

int node_try_compression(node *n, const void *key, uint32_t len)
{
  return node_try_prefix_compression(n, key, len);
}

void print_total_id()
{
  printf("%u\n", node_id);
}

#endif /* Test */
