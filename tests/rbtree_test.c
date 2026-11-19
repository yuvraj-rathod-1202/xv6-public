// Host-side unit test for rbtree.c (the exact file the kernel uses).
//
//   make rbtree-test        (or: gcc -O2 -Wall -o rbtree_test tests/rbtree_test.c rbtree.c)
//
// 1. Randomised differential test: millions of insert / erase / pop-min /
//    re-key operations, mirroring how the CFS run queue uses the tree,
//    checked against a trivially-correct reference (an "in tree" flag per
//    item plus linear scans). After every operation the tree must pass
//    rb_verify(), its minimum must equal the reference minimum, and its size
//    must match; periodically the full in-order sequence is compared too.
// 2. Height bound: a red-black tree with n nodes has height <= 2*log2(n+1).
// 3. Checker self-test: deliberately corrupt valid trees and make sure
//    rb_verify() reports each kind of corruption (a checker that never fails
//    proves nothing).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../rbtree.h"

struct item {
  struct rb_node node;
  unsigned long long key;   // like vruntime
  int id;                   // like pid: tie-breaker, unique
  int in;                   // reference: is it in the tree?
};

static int
item_less(struct rb_node *a, struct rb_node *b)
{
  struct item *x = rb_entry(a, struct item, node);
  struct item *y = rb_entry(b, struct item, node);
  if(x->key != y->key)
    return x->key < y->key;
  return x->id < y->id;
}

static int
ref_less(struct item *x, struct item *y)
{
  if(x->key != y->key)
    return x->key < y->key;
  return x->id < y->id;
}

static unsigned long long rng = 88172645463325252ULL;
static unsigned long long
xrand(void)
{
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return rng;
}

static int failures;
static long total_ops;

#define CHECK(cond, ...) do { \
  if(!(cond)){ \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); \
    failures++; return; } } while(0)

static int
height(struct rb_node *n)
{
  int l, r;
  if(!n)
    return 0;
  l = height(n->left);
  r = height(n->right);
  return 1 + (l > r ? l : r);
}

static int
cmp_items(const void *a, const void *b)
{
  struct item *x = *(struct item**)a, *y = *(struct item**)b;
  return ref_less(x, y) ? -1 : ref_less(y, x) ? 1 : 0;
}

// One randomised run with n items, ops operations, keys drawn from keyrange
// (small ranges force many equal keys, exercising the id tie-break).
static void
differential(int n, long ops, unsigned keyrange, int full_every)
{
  struct item *items = calloc(n, sizeof(*items));
  struct item **sorted = calloc(n, sizeof(*sorted));
  struct rb_root root = { 0 };
  long op;
  int size = 0, i, r;

  for(i = 0; i < n; i++)
    items[i].id = i;

  for(op = 0; op < ops; op++, total_ops++){
    int k = xrand() % 4;
    struct item *it = &items[xrand() % n];

    if(k == 0 || size == 0){            // insert
      if(!it->in){
        it->key = xrand() % keyrange;
        rb_insert(&root, &it->node, item_less);
        it->in = 1;
        size++;
      }
    } else if(k == 1){                  // erase an arbitrary element
      if(it->in){
        rb_erase(&root, &it->node);
        it->in = 0;
        size--;
      }
    } else if(k == 2){                  // pop the minimum (scheduler pick)
      struct rb_node *f = rb_first(&root);
      struct item *m = rb_entry(f, struct item, node);
      rb_erase(&root, f);
      m->in = 0;
      size--;
    } else {                            // re-key (run, charge, requeue)
      if(it->in){
        rb_erase(&root, &it->node);
        it->key += xrand() % (keyrange / 4 + 1);
        rb_insert(&root, &it->node, item_less);
      }
    }

    r = rb_verify(&root, item_less);
    CHECK(r >= 0, "n=%d op %ld: rb_verify: %s", n, op, rb_strerror(r));
    CHECK(r == size, "n=%d op %ld: size %d, reference %d", n, op, r, size);

    if(size > 0){
      struct item *m = 0, *t = rb_entry(rb_first(&root), struct item, node);
      for(i = 0; i < n; i++)
        if(items[i].in && (m == 0 || ref_less(&items[i], m)))
          m = &items[i];
      CHECK(t == m, "n=%d op %ld: rb_first is id %d, reference min is id %d",
            n, op, t->id, m->id);
    } else {
      CHECK(rb_first(&root) == 0, "n=%d op %ld: empty tree has a first node", n, op);
    }

    if(full_every && op % full_every == 0){
      struct rb_node *x;
      int m = 0;
      for(i = 0; i < n; i++)
        if(items[i].in)
          sorted[m++] = &items[i];
      qsort(sorted, m, sizeof(*sorted), cmp_items);
      i = 0;
      for(x = rb_first(&root); x; x = rb_next(x), i++)
        CHECK(i < m && rb_entry(x, struct item, node) == sorted[i],
              "n=%d op %ld: in-order position %d differs", n, op, i);
      CHECK(i == m, "n=%d op %ld: in-order length %d, reference %d", n, op, i, m);
    }
  }
  free(items);
  free(sorted);
}

static void
height_bound(int n)
{
  struct item *items = calloc(n, sizeof(*items));
  struct rb_root root = { 0 };
  int i, h, bound = 0;

  // Sequential keys are the worst case for an unbalanced BST.
  for(i = 0; i < n; i++){
    items[i].id = i;
    items[i].key = i;
    rb_insert(&root, &items[i].node, item_less);
  }
  for(i = n + 1; i > 1; i >>= 1)
    bound++;
  bound *= 2;   // 2 * floor(log2(n+1)) ... rounded up below
  bound += 2;
  h = height(root.node);
  CHECK(rb_verify(&root, item_less) == n, "sequential insert of %d invalid", n);
  CHECK(h <= bound, "height %d exceeds 2*log2(%d+1) bound %d", h, n, bound);
  free(items);
}

// Build a valid tree, damage it, and expect rb_verify to notice.
static void
checker_selftest(void)
{
  struct item items[15];
  struct rb_root root;
  struct rb_node *n, *a, *b;
  int i, r;

  #define BUILD() do { memset(items, 0, sizeof(items)); root.node = 0; \
    for(i = 0; i < 15; i++){ items[i].id = i; items[i].key = i; \
      rb_insert(&root, &items[i].node, item_less); } \
    CHECK(rb_verify(&root, item_less) == 15, "self-test base tree invalid"); } while(0)

  BUILD();
  root.node->color = RB_RED;
  r = rb_verify(&root, item_less);
  CHECK(r == RB_ERR_ROOT_RED, "red root not detected (%s)", rb_strerror(r));

  // Red-red ONLY: black root R with a red left child A whose left child B
  // is also red. Every root-to-leaf path still has one black node, so only
  // the red-red rule is broken and only RB_ERR_RED_RED may be reported.
  memset(items, 0, sizeof(items));
  for(i = 0; i < 3; i++){
    items[i].id = i;
    items[i].key = i;
  }
  root.node = &items[2].node;                       // R (key 2)
  items[2].node.color = RB_BLACK;
  items[2].node.left = &items[1].node;              // A (key 1)
  items[1].node.parent = &items[2].node;
  items[1].node.color = RB_RED;
  items[1].node.left = &items[0].node;              // B (key 0)
  items[0].node.parent = &items[1].node;
  items[0].node.color = RB_RED;
  r = rb_verify(&root, item_less);
  CHECK(r == RB_ERR_RED_RED, "red-red not detected (%s)", rb_strerror(r));

  BUILD();   // flip one black leaf-level node to red under a black parent
  for(n = rb_first(&root); n; n = rb_next(n))
    if(n->color == RB_BLACK && n != root.node && !n->left && !n->right &&
       n->parent->color == RB_BLACK)
      break;
  if(n){
    n->color = RB_RED;
    r = rb_verify(&root, item_less);
    CHECK(r == RB_ERR_BLACK_HT, "black-height change not detected (%s)",
          rb_strerror(r));
  }

  BUILD();   // swap two keys so the order is wrong
  a = rb_first(&root);
  b = rb_next(rb_next(a));
  {
    unsigned long long t = rb_entry(a, struct item, node)->key;
    rb_entry(a, struct item, node)->key = rb_entry(b, struct item, node)->key;
    rb_entry(b, struct item, node)->key = t;
  }
  r = rb_verify(&root, item_less);
  CHECK(r == RB_ERR_ORDER, "order violation not detected (%s)", rb_strerror(r));

  BUILD();
  root.node->left->parent = root.node->right;
  r = rb_verify(&root, item_less);
  CHECK(r == RB_ERR_PARENT, "bad parent not detected (%s)", rb_strerror(r));

  BUILD();
  root.node->left->color = 7;
  r = rb_verify(&root, item_less);
  CHECK(r == RB_ERR_COLOR, "bad colour not detected (%s)", rb_strerror(r));
}

int
main(int argc, char *argv[])
{
  long scale = argc > 1 ? atol(argv[1]) : 1;

  checker_selftest();
  // Small trees, like the run queue with a handful of processes.
  differential(8, 200000 * scale, 4, 50);
  // NPROC-sized trees with many equal vruntimes.
  differential(256, 1000000 * scale, 64, 1000);
  // Wide key range.
  differential(256, 500000 * scale, 1u << 30, 5000);
  // Large tree, beyond what the kernel needs.
  differential(5000, 100000 * scale, 1u << 20, 20000);
  height_bound(1);
  height_bound(1000);
  height_bound(100000);

  if(failures){
    printf("rbtree_test: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("rbtree_test: ALL TESTS PASSED (%ld operations, each verified)\n",
         total_ops);
  return 0;
}
