#!/bin/sh
# Mutation test for tests/rbtree_test.c: build the unit test against
# deliberately broken copies of rbtree.c. Every mutant must make the test
# fail (or crash); a mutant that passes means the unit test has a blind spot.
#
#   sh tests/rbtree_mutation.sh        (run from the repository root)
set -u
cd "$(dirname "$0")/.." || exit 1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/tests"
cp rbtree.h "$work/"
cp tests/rbtree_test.c "$work/tests/"

survived=0
total=0
mutant() {   # $1 = description, $2 = sed script applied to rbtree.c
  total=$((total + 1))
  sed "$2" rbtree.c > "$work/rbtree.c"
  if cmp -s rbtree.c "$work/rbtree.c"; then
    echo "  ERROR  mutation did not change the code: $1"
    survived=$((survived + 1))
    return
  fi
  if ! gcc -O2 -w -o "$work/t" "$work/tests/rbtree_test.c" "$work/rbtree.c"; then
    echo "  ERROR  mutant does not compile: $1"
    survived=$((survived + 1))
    return
  fi
  if timeout 120 "$work/t" >/dev/null 2>&1; then
    echo "  SURVIVED  $1"
    survived=$((survived + 1))
  else
    echo "  killed    $1"
  fi
}

mutant "insert case 1 skips uncle recolour"    '0,/        u->color = RB_BLACK;/s//        ;/'
mutant "insert case 3 skips parent recolour"   '0,/        p->color = RB_BLACK;  \/\/ case 3/s//        ;/'
mutant "insert never blackens the root"        's/^  root->node->color = RB_BLACK;$/  ;/'
mutant "insert descends the wrong way"         's/x = less(z, x) ? x->left : x->right;/x = less(z, x) ? x->right : x->left;/'
mutant "erase case 4 skips sibling's child"    '0,/        w->right->color = RB_BLACK;/s//        ;/'
mutant "erase skips the fixup"                 's/^    erase_fixup(root, x, xp);$/    ;/'
mutant "erase forgets the successor colour"    's/^    y->color = z->color;$/    ;/'
mutant "rotate_left drops a parent link"       '0,/    y->left->parent = x;/s//    ;/'
mutant "transplant drops the parent update"    's/^    v->parent = u->parent;$/    ;/'
mutant "rb_first returns the root"             '0,/  while(n->left)/s//  while(0)/'
mutant "verify ignores red-red"                's/^    return RB_ERR_RED_RED;$/    ;/'
mutant "verify ignores black height"           's/^  if(lh != rh)$/  if(0)/'

echo "rbtree_mutation: $((total - survived))/$total mutants killed"
[ "$survived" -eq 0 ]
