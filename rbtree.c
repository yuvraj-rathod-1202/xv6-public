// Generic intrusive red-black tree. See rbtree.h.
// Null children are the black leaves of CLRS; no sentinel node is used.

#include "rbtree.h"

static int
color(struct rb_node *n)
{
  return n ? n->color : RB_BLACK;
}

static void
rotate_left(struct rb_root *root, struct rb_node *x)
{
  struct rb_node *y = x->right;

  x->right = y->left;
  if(y->left)
    y->left->parent = x;
  y->parent = x->parent;
  if(x->parent == 0)
    root->node = y;
  else if(x == x->parent->left)
    x->parent->left = y;
  else
    x->parent->right = y;
  y->left = x;
  x->parent = y;
}

static void
rotate_right(struct rb_root *root, struct rb_node *x)
{
  struct rb_node *y = x->left;

  x->left = y->right;
  if(y->right)
    y->right->parent = x;
  y->parent = x->parent;
  if(x->parent == 0)
    root->node = y;
  else if(x == x->parent->right)
    x->parent->right = y;
  else
    x->parent->left = y;
  y->right = x;
  x->parent = y;
}

static void
insert_fixup(struct rb_root *root, struct rb_node *z)
{
  struct rb_node *p, *g, *u;

  while((p = z->parent) != 0 && p->color == RB_RED){
    g = p->parent;            // exists: a red node is never the root
    if(p == g->left){
      u = g->right;
      if(color(u) == RB_RED){ // case 1: recolour and move up
        p->color = RB_BLACK;
        u->color = RB_BLACK;
        g->color = RB_RED;
        z = g;
      } else {
        if(z == p->right){    // case 2: rotate into case 3
          z = p;
          rotate_left(root, z);
          p = z->parent;
        }
        p->color = RB_BLACK;  // case 3
        g->color = RB_RED;
        rotate_right(root, g);
      }
    } else {
      u = g->left;
      if(color(u) == RB_RED){
        p->color = RB_BLACK;
        u->color = RB_BLACK;
        g->color = RB_RED;
        z = g;
      } else {
        if(z == p->left){
          z = p;
          rotate_right(root, z);
          p = z->parent;
        }
        p->color = RB_BLACK;
        g->color = RB_RED;
        rotate_left(root, g);
      }
    }
  }
  root->node->color = RB_BLACK;
}

void
rb_insert(struct rb_root *root, struct rb_node *z, rb_less_fn less)
{
  struct rb_node *parent = 0, *x = root->node;

  while(x){
    parent = x;
    x = less(z, x) ? x->left : x->right;
  }
  z->parent = parent;
  z->left = 0;
  z->right = 0;
  z->color = RB_RED;
  if(parent == 0)
    root->node = z;
  else if(less(z, parent))
    parent->left = z;
  else
    parent->right = z;
  insert_fixup(root, z);
}

// Replace subtree u with subtree v (v may be null).
static void
transplant(struct rb_root *root, struct rb_node *u, struct rb_node *v)
{
  if(u->parent == 0)
    root->node = v;
  else if(u == u->parent->left)
    u->parent->left = v;
  else
    u->parent->right = v;
  if(v)
    v->parent = u->parent;
}

// x carries an extra black; xp is its parent (needed because x may be null).
static void
erase_fixup(struct rb_root *root, struct rb_node *x, struct rb_node *xp)
{
  struct rb_node *w;

  while(x != root->node && color(x) == RB_BLACK){
    if(x == xp->left){
      w = xp->right;          // non-null: x's side is short one black
      if(color(w) == RB_RED){ // case 1
        w->color = RB_BLACK;
        xp->color = RB_RED;
        rotate_left(root, xp);
        w = xp->right;
      }
      if(color(w->left) == RB_BLACK && color(w->right) == RB_BLACK){
        w->color = RB_RED;    // case 2: push the extra black up
        x = xp;
        xp = x->parent;
      } else {
        if(color(w->right) == RB_BLACK){  // case 3
          w->left->color = RB_BLACK;
          w->color = RB_RED;
          rotate_right(root, w);
          w = xp->right;
        }
        w->color = xp->color;             // case 4
        xp->color = RB_BLACK;
        w->right->color = RB_BLACK;
        rotate_left(root, xp);
        x = root->node;
        xp = 0;
      }
    } else {
      w = xp->left;
      if(color(w) == RB_RED){
        w->color = RB_BLACK;
        xp->color = RB_RED;
        rotate_right(root, xp);
        w = xp->left;
      }
      if(color(w->right) == RB_BLACK && color(w->left) == RB_BLACK){
        w->color = RB_RED;
        x = xp;
        xp = x->parent;
      } else {
        if(color(w->left) == RB_BLACK){
          w->right->color = RB_BLACK;
          w->color = RB_RED;
          rotate_left(root, w);
          w = xp->left;
        }
        w->color = xp->color;
        xp->color = RB_BLACK;
        w->left->color = RB_BLACK;
        rotate_right(root, xp);
        x = root->node;
        xp = 0;
      }
    }
  }
  if(x)
    x->color = RB_BLACK;
}

void
rb_erase(struct rb_root *root, struct rb_node *z)
{
  struct rb_node *y = z, *x, *xp;
  int ycolor = y->color;

  if(z->left == 0){
    x = z->right;
    xp = z->parent;
    transplant(root, z, z->right);
  } else if(z->right == 0){
    x = z->left;
    xp = z->parent;
    transplant(root, z, z->left);
  } else {
    y = z->right;             // successor: leftmost of right subtree
    while(y->left)
      y = y->left;
    ycolor = y->color;
    x = y->right;
    if(y->parent == z){
      xp = y;
    } else {
      xp = y->parent;
      transplant(root, y, y->right);
      y->right = z->right;
      y->right->parent = y;
    }
    transplant(root, z, y);
    y->left = z->left;
    y->left->parent = y;
    y->color = z->color;
  }
  z->left = 0;
  z->right = 0;
  z->parent = 0;
  if(ycolor == RB_BLACK)
    erase_fixup(root, x, xp);
}

struct rb_node*
rb_first(struct rb_root *root)
{
  struct rb_node *n = root->node;

  if(n == 0)
    return 0;
  while(n->left)
    n = n->left;
  return n;
}

struct rb_node*
rb_next(struct rb_node *n)
{
  struct rb_node *p;

  if(n->right){
    n = n->right;
    while(n->left)
      n = n->left;
    return n;
  }
  p = n->parent;
  while(p && n == p->right){
    n = p;
    p = p->parent;
  }
  return p;
}

// Returns the black height of the subtree at n, or a negative RB_ERR_*.
// lo/hi bound the keys allowed in this subtree (null = unbounded).
static int
verify(struct rb_node *n, struct rb_node *parent, rb_less_fn less,
       struct rb_node *lo, struct rb_node *hi, int depth, int *count)
{
  int lh, rh;

  if(n == 0)
    return 1;
  if(depth > 128)
    return RB_ERR_DEPTH;
  if(n->parent != parent)
    return RB_ERR_PARENT;
  if(n->color != RB_RED && n->color != RB_BLACK)
    return RB_ERR_COLOR;
  if(n->color == RB_RED &&
     (color(n->left) == RB_RED || color(n->right) == RB_RED))
    return RB_ERR_RED_RED;
  if((lo && less(n, lo)) || (hi && less(hi, n)))
    return RB_ERR_ORDER;
  lh = verify(n->left, n, less, lo, n, depth + 1, count);
  if(lh < 0)
    return lh;
  rh = verify(n->right, n, less, n, hi, depth + 1, count);
  if(rh < 0)
    return rh;
  if(lh != rh)
    return RB_ERR_BLACK_HT;
  (*count)++;
  return lh + (n->color == RB_BLACK);
}

int
rb_verify(struct rb_root *root, rb_less_fn less)
{
  int count = 0, r;

  if(root->node == 0)
    return 0;
  if(root->node->color != RB_BLACK)
    return RB_ERR_ROOT_RED;
  r = verify(root->node, 0, less, 0, 0, 0, &count);
  return r < 0 ? r : count;
}

const char*
rb_strerror(int err)
{
  switch(err){
  case RB_ERR_ROOT_RED: return "root is red";
  case RB_ERR_RED_RED:  return "red node has a red child";
  case RB_ERR_BLACK_HT: return "unequal black heights";
  case RB_ERR_PARENT:   return "bad parent pointer";
  case RB_ERR_ORDER:    return "keys out of order";
  case RB_ERR_COLOR:    return "invalid colour";
  case RB_ERR_DEPTH:    return "too deep (cycle?)";
  default:              return err >= 0 ? "ok" : "unknown error";
  }
}
