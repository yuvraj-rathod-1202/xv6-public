// Generic intrusive red-black tree (CLRS, chapter 13).
//
// A tree node is embedded in the containing structure (struct proc embeds
// one for the CFS run queue); rb_entry() recovers the container. The code has
// no xv6 dependencies, so the very same rbtree.c is also compiled on the host
// by tests/rbtree_test.c and checked against a reference implementation.

#ifndef XV6_RBTREE_H
#define XV6_RBTREE_H

#define RB_RED   0
#define RB_BLACK 1

struct rb_node {
  struct rb_node *left;
  struct rb_node *right;
  struct rb_node *parent;
  int color;
};

struct rb_root {
  struct rb_node *node;
};

// Strict weak ordering: nonzero if a sorts before b. Keys must be unique
// (break ties, e.g. by pid) for rb_verify's ordering check to be exact.
typedef int (*rb_less_fn)(struct rb_node *a, struct rb_node *b);

#define rb_entry(ptr, type, member) \
  ((type*)((char*)(ptr) - __builtin_offsetof(type, member)))

void            rb_insert(struct rb_root *root, struct rb_node *n, rb_less_fn less);
void            rb_erase(struct rb_root *root, struct rb_node *n);
struct rb_node* rb_first(struct rb_root *root);
struct rb_node* rb_next(struct rb_node *n);

// rb_verify checks every red-black and binary-search-tree property:
// root is black, no red node has a red child, every root-to-leaf path has
// the same number of black nodes, parent pointers are consistent, and the
// in-order sequence is sorted. Returns the number of nodes (>= 0) if the
// tree is valid, or one of the negative RB_ERR_* codes.
#define RB_ERR_ROOT_RED  (-1)
#define RB_ERR_RED_RED   (-2)
#define RB_ERR_BLACK_HT  (-3)
#define RB_ERR_PARENT    (-4)
#define RB_ERR_ORDER     (-5)
#define RB_ERR_COLOR     (-6)
#define RB_ERR_DEPTH     (-7)   // deeper than any valid tree: a cycle

int             rb_verify(struct rb_root *root, rb_less_fn less);
const char*     rb_strerror(int err);

#endif // XV6_RBTREE_H
