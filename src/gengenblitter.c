 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Optimized blitter minterm function generator
  *
  * Copyright 1995,1996 Bernd Schmidt
  * Copyright 1996 Alessandro Bissacco
  *
  * Overkill, n: cf. genblitter
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"

static void nop(int);

#define xmalloc malloc
#define xfree free
#define xrealloc realloc

typedef struct tree_n {
    enum tree_op { op_and, op_or, op_xor, op_not, op_a, op_b, op_c, op_d, op_e, op_f } op;
    struct tree_n *left, *right;
} *tree;

static struct tree_n TRA = { op_a, NULL, NULL };
static struct tree_n TRB = { op_b, NULL, NULL };
static struct tree_n TRC = { op_c, NULL, NULL };
static struct tree_n TRD = { op_d, NULL, NULL };
static struct tree_n TRE = { op_e, NULL, NULL };
static struct tree_n TRF = { op_f, NULL, NULL };
static tree tree_a = &TRA;
static tree tree_b = &TRB;
static tree tree_c = &TRC;
static tree tree_d = &TRD;
static tree tree_e = &TRE;
static tree tree_f = &TRF;

typedef struct {
    tree *trees;
    int space;
    int ntrees;
} tree_vec;

/****i* januae-gengenblitter/issrc ***************************************
*
*   NAME
*       issrc() -- Identify a built-in expression leaf.
*
*   SYNOPSIS
*       STATIC_INLINE int issrc (tree t);
*
*   FUNCTION
*       Compare the node pointer with the six built-in source leaves.
*
*   INPUTS
*       t - Expression node to inspect.
*
*   RESULT
*       Nonzero for the six static source nodes; zero otherwise.
*
*   NOTES
*       The test uses node identity rather than the operation field.
*
****************************************************************************
*
*/

STATIC_INLINE int issrc (tree t)
{
    return t == tree_a || t == tree_b || t == tree_c || t == tree_d || t == tree_e || t == tree_f;
}

/****i* januae-gengenblitter/new_op_tree *********************************
*
*   NAME
*       new_op_tree() -- Construct an expression operation node.
*
*   SYNOPSIS
*       static tree new_op_tree(enum tree_op op, tree l, tree r);
*
*   FUNCTION
*       Eliminate adjacent NOT operations, or allocate an operation node
*       that refers to the supplied operands.
*
*   INPUTS
*       op - Operation for the new node.
*       l - Left operand.
*       r - Right operand.
*
*   RESULT
*       Allocated tree node, or the inner operand when two NOT operations cancel.
*
*   NOTES
*       The double-negation case frees the outer NOT node l. Other
*       nodes are allocated with malloc without an allocation check.
*
****************************************************************************
*
*/

static tree new_op_tree(enum tree_op op, tree l, tree r)
{
    tree t;
    if (op == op_not && l->op == op_not) {
	t = l->left;
	xfree(l);
	return t;
    }
    t = (tree)xmalloc(sizeof(struct tree_n));
    t->left = l;
    t->right = r;
    t->op = op;
    return t;
}

/****i* januae-gengenblitter/opidx ***************************************
*
*   NAME
*       opidx() -- Map a source leaf to its index.
*
*   SYNOPSIS
*       static int opidx (tree t);
*
*   FUNCTION
*       Interpret the operation code of a source leaf.
*
*   INPUTS
*       t - Expression node.
*
*   RESULT
*       0 through 5 for leaves A through F; -1 for other operations.
*
****************************************************************************
*
*/

static int opidx (tree t)
{
    switch (t->op) {
     case op_a:
	return 0;
     case op_b:
	return 1;
     case op_c:
	return 2;
     case op_d:
	return 3;
     case op_e:
	return 4;
     case op_f:
	return 5;
     default:
	return -1;
    }
}

/****i* januae-gengenblitter/tree_cst ************************************
*
*   NAME
*       tree_cst() -- Accumulate expression operation costs.
*
*   SYNOPSIS
*       static int tree_cst (tree t, unsigned int *src, unsigned int *notsrc);
*
*   FUNCTION
*       Walk the expression recursively and mark direct and inverted
*       source references while accumulating operation weights.
*
*   INPUTS
*       t - Expression to examine.
*       src - Six-element array recording direct source references.
*       notsrc - Six-element array recording inverted source references.
*
*   RESULT
*       Sum of node costs; source leaves contribute zero, NOT contributes
*       three, and binary operations contribute four each.
*
*   NOTES
*       The arrays are updated but not cleared. Unsupported node operations
*       call abort().
*
****************************************************************************
*
*/

static int tree_cst (tree t, unsigned int *src, unsigned int *notsrc)
{
    int idx = opidx (t);
    if (idx >= 0) {
	src[idx] = 1;
	return 0;
    }
    switch (t->op) {
     case op_not:
	idx = opidx (t->left);
	if (idx >= 0) {
	    notsrc[idx] = 1;
	    return 3;
	}
	return 3 + tree_cst (t->left, src, notsrc);

     case op_and:
     case op_xor:
     case op_or:
	return 4 + tree_cst (t->left, src, notsrc) + tree_cst (t->right, src, notsrc);
	
     default:
	abort ();
    }
}

/****i* januae-gengenblitter/tree_cost ***********************************
*
*   NAME
*       tree_cost() -- Estimate the cost of a complete expression tree.
*
*   SYNOPSIS
*       static int tree_cost (tree t);
*
*   FUNCTION
*       Use fresh source-reference masks for one complete tree
*       and apply a penalty for inputs used in both polarities.
*
*   INPUTS
*       t - Expression tree.
*
*   RESULT
*       Operation cost plus one for each source referenced both directly
*       and inverted.
*
*   NOTES
*       The direct and inverted source masks are initialized by this call.
*
****************************************************************************
*
*/

static int tree_cost (tree t)
{
    int i, cost;
    unsigned int src[6], notsrc[6];
    memset (src, 0, sizeof src);
    memset (notsrc, 0, sizeof notsrc);

    cost = tree_cst (t, src, notsrc);
    for (i = 0; i < 6; i++)
	if (src[i] && notsrc[i])
	    cost++;
    return cost;
}

/****i* januae-gengenblitter/add_vec *************************************
*
*   NAME
*       add_vec() -- Append a tree to a candidate vector.
*
*   SYNOPSIS
*       static int add_vec(tree_vec *tv, tree t);
*
*   FUNCTION
*       Append a node pointer, extending the vector allocation when full.
*
*   INPUTS
*       tv - Candidate vector.
*       t - Node to append.
*
*   RESULT
*       Always returns 1.
*
*   NOTES
*       Capacity grows by 40 entries through realloc(). Allocation failure
*       is not handled; the vector stores the node pointer, not a copy.
*
****************************************************************************
*
*/

static int add_vec(tree_vec *tv, tree t)
{
    int i;
#if 0
    if (! tree_isnormal(t))
	nop(2);
#endif
    if (tv->ntrees == tv->space) {
	tv->trees = (tree *)xrealloc(tv->trees, sizeof(tree)*(tv->space += 40));
    }
    tv->trees[tv->ntrees++] = t;

    return 1;
}

/****i* januae-gengenblitter/init_vec ************************************
*
*   NAME
*       init_vec() -- Initialize an empty candidate vector.
*
*   SYNOPSIS
*       static void init_vec(tree_vec *tv);
*
*   FUNCTION
*       Reset the vector representation to its empty state.
*
*   INPUTS
*       tv - Vector to initialize.
*
*   RESULT
*       None. The vector pointer and both counts are reset to zero.
*
*   NOTES
*       Does not release an existing allocation.
*
****************************************************************************
*
*/

static void init_vec(tree_vec *tv)
{
    tv->ntrees = tv->space = 0;
    tv->trees = NULL;
}

/****i* januae-gengenblitter/do_sprint_tree ******************************
*
*   NAME
*       do_sprint_tree() -- Append an expression as C syntax.
*
*   SYNOPSIS
*       static void do_sprint_tree (char *s, tree t);
*
*   FUNCTION
*       Recursively render source identifiers and C bitwise operators
*       into the existing destination string.
*
*   INPUTS
*       s - NUL-terminated destination string with sufficient remaining space.
*       t - Expression tree to print.
*
*   RESULT
*       None. Appends source names and parenthesized bitwise expressions to s.
*
*   NOTES
*       Uses strcat() without checking the destination capacity. Nested
*       right-hand binary operations of the same kind are printed in sequence.
*
****************************************************************************
*
*/

static void do_sprint_tree (char *s, tree t)
{
    enum tree_op op = t->op;
    switch (op) {
     case op_a:
	strcat (s, "srca");
	break;
     case op_b:
	strcat (s, "srcb");
	break;
     case op_c:
	strcat (s, "srcc");
	break;
     case op_d:
	strcat (s, "srcd");
	break;
     case op_e:
	strcat (s, "srce");
	break;
     case op_f:
	strcat (s, "srcf");
	break;

     case op_and:
     case op_or:
     case op_xor:
	{
	    
	    char *c = op == op_and ? " & " : op == op_or ? " | " : " ^ ";
	    strcat (s, "(");
	    do_sprint_tree (s, t->left);
	    strcat (s, c);
	    while (t->right->op == op) {
		t = t->right;
		do_sprint_tree (s, t->left);
		strcat (s, c);
	    }
	    do_sprint_tree(s, t->right);
	    strcat (s, ")");
	}
	break;

     case op_not:
	strcat (s, "~");
	do_sprint_tree (s, t->left);
	break;
    }
}

static tree_vec size_trees[20];

static struct tree_n bad_tree = { op_and, &bad_tree, &bad_tree };

static unsigned int used_mask[256];
static tree best_trees[256];
static unsigned int best_cost[256];
static int n_unknown;

/****i* januae-gengenblitter/which_fn ************************************
*
*   NAME
*       which_fn() -- Evaluate an expression as a three-input truth table.
*
*   SYNOPSIS
*       static unsigned long which_fn (tree t);
*
*   FUNCTION
*       Compose fixed three-input truth-table masks through
*       the Boolean operations in the tree.
*
*   INPUTS
*       t - Expression over A, B and C using AND, OR, XOR and NOT.
*
*   RESULT
*       Eight-bit truth-table pattern in an unsigned long.
*
*   NOTES
*       Unsupported leaf or operation types call abort().
*
****************************************************************************
*
*/

static unsigned long which_fn (tree t)
{
    switch (t->op) {
     case op_a:
	return 0xf0;
     case op_b:
	return 0xcc;
     case op_c:
	return 0xaa;
     case op_and:
	return which_fn (t->left) & which_fn (t->right);
     case op_or:
	return which_fn (t->left) | which_fn (t->right);
     case op_xor:
	return which_fn (t->left) ^ which_fn (t->right);
     case op_not:
	return 0xFF & ~which_fn (t->left);
     default:
	abort ();
    }
}

/****i* januae-gengenblitter/tree_used_mask ******************************
*
*   NAME
*       tree_used_mask() -- Find the inputs used by a Boolean expression.
*
*   SYNOPSIS
*       static unsigned long tree_used_mask (tree t);
*
*   FUNCTION
*       Combine input-usage bits across all tree operands.
*
*   INPUTS
*       t - Expression over A, B and C.
*
*   RESULT
*       Bit mask with A, B and C represented by bits 0, 1 and 2.
*
*   NOTES
*       Unsupported leaf or operation types call abort().
*
****************************************************************************
*
*/

static unsigned long tree_used_mask (tree t)
{
    switch (t->op) {
     case op_a:
	return 1;
     case op_b:
	return 2;
     case op_c:
	return 4;
     case op_and:
     case op_or:
     case op_xor:
	return tree_used_mask (t->left) | tree_used_mask (t->right);
     case op_not:
	return tree_used_mask (t->left);
     default:
	abort ();
    }
}

/****i* januae-gengenblitter/candidate ***********************************
*
*   NAME
*       candidate() -- Record a candidate expression for its truth table.
*
*   SYNOPSIS
*       static void candidate (tree_vec *v, tree t);
*
*   FUNCTION
*       Index the truth-table entry for t, compare estimated cost
*       with the recorded best candidate and retain t in the vector.
*
*   INPUTS
*       v - Candidate vector.
*       t - Expression to consider.
*
*   RESULT
*       None. Appends t and updates the best expression and cost for its
*       truth-table value when the cost is lower.
*
*   NOTES
*       Decrements n_unknown the first time the table value is encountered.
*       Does not copy or release the candidate node.
*
****************************************************************************
*
*/

static void candidate (tree_vec *v, tree t)
{
    unsigned long fn = which_fn (t);
    unsigned int cost = tree_cost (t);
    if (best_trees[fn] == 0)
	n_unknown--;
    if (cost < best_cost[fn])
	best_trees[fn] = t, best_cost[fn] = cost;
    add_vec (v, t);
}

/****i* januae-gengenblitter/cand_and_not ********************************
*
*   NAME
*       cand_and_not() -- Consider an expression and its complement.
*
*   SYNOPSIS
*       static void cand_and_not (tree_vec *v, tree t);
*
*   FUNCTION
*       Submit both t and a newly constructed complement for selection.
*
*   INPUTS
*       v - Candidate vector.
*       t - Expression to consider.
*
*   RESULT
*       None. Adds t and a NOT expression constructed from t.
*
*   NOTES
*       Both candidates may share operand nodes.
*
****************************************************************************
*
*/

static void cand_and_not (tree_vec *v, tree t)
{
    candidate (v, t);
    t = new_op_tree (op_not, t, 0);
    candidate (v, t);
}

/****i* januae-gengenblitter/try_tree ************************************
*
*   NAME
*       try_tree() -- Discard a redundant or costly expression candidate.
*
*   SYNOPSIS
*       static void try_tree (tree_vec *v, tree t);
*
*   FUNCTION
*       Apply truth-table, source-use and cost rejection criteria
*       before retaining a binary candidate.
*
*   INPUTS
*       v - Candidate vector.
*       t - Binary expression candidate.
*
*   RESULT
*       None. Retained candidates and their complements are appended to v.
*
*   NOTES
*       Rejects results equal to either operand, constant results, inputs
*       outside the required mask, and candidates exceeding the current best
*       cost by more than six. Frees only the rejected root node.
*
****************************************************************************
*
*/

static void try_tree (tree_vec *v, tree t)
{
    int fnl = which_fn (t->left);
    int fnr = which_fn (t->right);
    int fn = which_fn (t);
    if (fn == fnl
	|| fn == fnr
	|| fn == 0
	|| fn == 0xFF
	|| (tree_used_mask (t) & ~used_mask[fn]) != 0
	|| best_cost[fn] + 6 < tree_cost (t))
    {
	xfree (t);
	return;
    }
    cand_and_not (v, t);
}

/****i* januae-gengenblitter/find_best_trees *****************************
*
*   NAME
*       find_best_trees() -- Select Boolean expressions for nonconstant minterms.
*
*   SYNOPSIS
*       static void find_best_trees (void);
*
*   FUNCTION
*       Enumerate combinations of candidate expressions by tree size
*       and record the lowest-cost representative encountered.
*
*   INPUTS
*       None. Uses the global used_mask table.
*
*   RESULT
*       None. Populates best_trees and best_cost for the truth-table values
*       encountered during the bounded candidate search.
*
*   NOTES
*       The constant minterms use a sentinel instead of an expression.
*       Candidate sizes are explored only while smaller than 20;
*       this function does not free its candidate allocations.
*
****************************************************************************
*
*/

static void find_best_trees (void)
{
    int i, size, do_stop;
    for (i = 0; i < 256; i++) {
	best_trees[i] = i == 0 || i == 255 ? &bad_tree : 0;
	best_cost[i] = 65535;
    }
    n_unknown = 254;

    init_vec (size_trees);
    cand_and_not (size_trees, tree_a);
    cand_and_not (size_trees, tree_b);
    cand_and_not (size_trees, tree_c);

    do_stop = 0;
    for (size = 2; ! do_stop && size < 20; size++) {
	int split, last_split;
	tree_vec *sv = size_trees + size - 1;
	
	if (n_unknown == 0)
	    do_stop = 1;
	last_split = (size >> 1) + 1;
	for (split = 1; split < last_split; split++) {
	    int szl = split;
	    int szr = size - split;
	    tree_vec *lv = size_trees + szl - 1;
	    tree_vec *rv = size_trees + szr - 1;
	    int i;

	    for (i = 0; i < lv->ntrees; i++) {
		tree l = lv->trees[i];
		int j;
		for (j = szl == szr ? i + 1 : 0; j < rv->ntrees; j++) {
		    tree r = rv->trees[j];

		    if (l->op != op_and || r->op != op_and) {
			tree tmp = (l->op == op_and
				    ? new_op_tree (op_and, r, l)
				    : new_op_tree (op_and, l, r));
			try_tree (sv, tmp);
		    }
		    if (l->op != op_or || r->op != op_or) {
			tree tmp = (l->op == op_or
				    ? new_op_tree (op_or, r, l)
				    : new_op_tree (op_or, l, r));
			try_tree (sv, tmp);
		    }
		    if (l->op != op_xor || r->op != op_xor) {
			tree tmp = (l->op == op_xor
				    ? new_op_tree (op_xor, r, l)
				    : new_op_tree (op_xor, l, r));
			try_tree (sv, tmp);
		    }
		}
	    }
	}
	/* An additional pass doesn't seem to create better solutions
	 * (not that much of a surprise).  */
	if (n_unknown == 0)
	    do_stop = 1;
    }
}

/****i* januae-gengenblitter/bitset **************************************
*
*   NAME
*       bitset() -- Test one bit of a minterm value.
*
*   SYNOPSIS
*       static int bitset (int mt, int bit);
*
*   FUNCTION
*       Apply a bit mask at the requested position.
*
*   INPUTS
*       mt - Minterm value.
*       bit - Bit index to inspect.
*
*   RESULT
*       Masked bit value, or zero when that bit is clear.
*
*   NOTES
*       The caller uses bit positions 0 through 7.
*
****************************************************************************
*
*/

static int bitset (int mt, int bit)
{
    return mt & (1 << bit);
}

/****i* januae-gengenblitter/generate_expr *******************************
*
*   NAME
*       generate_expr() -- Determine the input mask needed by a minterm.
*
*   SYNOPSIS
*       static unsigned int generate_expr (int minterm);
*
*   FUNCTION
*       Combine covered truth-table entries into implicant groups
*       and record which input bits are needed by those groups.
*
*   INPUTS
*       minterm - Eight-bit Boolean truth-table value.
*
*   RESULT
*       Mask of A, B and C inputs retained by the computed implicant
*       groups. Returns zero for constant minterms.
*
*   NOTES
*       The returned mask is used to discard candidate expressions
*       that reference unnecessary inputs.
*
****************************************************************************
*
*/

static unsigned int generate_expr (int minterm)
{
    int bits = 0;
    int i;
    int expr_dc[8], nexp = 0;
    int expr_used[8];

    if (minterm == 0 || minterm == 0xFF)
	return 0;

    for (i = 0; i < 8; i++) {
	if (bitset (minterm, i) && !bitset (bits, i)) {
	    int j;
	    int dontcare = 0;
	    int firstand = 1;
	    int bitbucket[8], bitcount;

	    bits |= 1<<i;
	    bitcount = 1; bitbucket[0] = i;
	    for(j=1; j<8; j *= 2) {
		int success = 1;
		int k;
		for(k=0; k < bitcount; k++) {
		    if (!bitset (minterm, bitbucket[k] ^ j)) {
			success = 0;
		    }
		}
		if (success) {
		    int l;
		    dontcare |= j;
		    for(l=bitcount; l < bitcount*2; l++) {
			bitbucket[l] = bitbucket[l-bitcount] ^ j;
			bits |= 1 << bitbucket[l];
		    }
		    bitcount *= 2;
		}
	    }
	    expr_used[nexp] = 1;
	    expr_dc[nexp] = dontcare;
	    nexp++;
	}
    }

    {
	unsigned int result = 0;
	for (i = 0; i < nexp; i++) {
	    int j;

	    for (j = 1; j < 8; j *= 2) {
		if (!(expr_dc[i] & j))
		    result |= (j == 1 ? 4 : j == 2 ? 2 : 1);
	    }
	}
	return result;
    }
}

/****i* januae-gengenblitter/print_tree **********************************
*
*   NAME
*       print_tree() -- Write an expression to standard output.
*
*   SYNOPSIS
*       static void print_tree(tree t);
*
*   FUNCTION
*       Render the tree into a local string buffer and print it.
*
*   INPUTS
*       t - Expression tree.
*
*   RESULT
*       None. Writes the expression without a trailing newline.
*
*   NOTES
*       Formats into a fixed 300-byte local buffer without bounds checking.
*
****************************************************************************
*
*/

static void print_tree(tree t)
{
    char buf[300] = "";
    do_sprint_tree (buf, t);
    printf ("%s", buf);
}

/****i* januae-gengenblitter/generate_optable ****************************
*
*   NAME
*       generate_optable() -- Emit the C blitter-operation lookup table.
*
*   SYNOPSIS
*       static void generate_optable(void);
*
*   FUNCTION
*       Emit 256 C initializer entries, each containing the chosen
*       expression text and its required-input mask.
*
*   INPUTS
*       None. Reads best_trees and used_mask.
*
*   RESULT
*       None. Writes a definition of blitops[256] to standard output.
*
*   NOTES
*       Outputs literal expressions for minterms 0 and 255 and prints
*       the chosen tree for other entries. Output errors are not checked.
*
****************************************************************************
*
*/

static void generate_optable(void)
{
    int minterm;
    printf(" /* This file generated automatically - do not edit */\n\n");
    printf("#include \"genblitter.h\"\n\n");
    printf("struct blitop blitops[256] = {\n");
    for (minterm = 0; minterm < 256; minterm++) {
	printf(" /* %02x */  { \"", minterm);
	if (minterm == 0)
	    printf ("0");
	else if (minterm == 255)
	    printf ("0xFFFFFFFF");
	else
	    print_tree (best_trees[minterm]);

	printf("\", %d }%s\n", used_mask[minterm], minterm == 255 ? "" : ",");
	fflush(stdout);
    }
    printf("};\n");
}

/****f* januae-gengenblitter/main ****************************************
*
*   NAME
*       main() -- Generate the optimized blitter-operation table.
*
*   SYNOPSIS
*       int main (int argc, char **argv);
*
*   FUNCTION
*       Compute required-input masks, search for candidate expressions
*       and print the resulting blitter table.
*
*   INPUTS
*       argc - Argument count (unused).
*       argv - Argument vector (unused).
*
*   RESULT
*       Returns zero after emitting the table.
*
*   NOTES
*       Computes input masks, selects candidate trees and writes C source
*       to standard output. Allocation and output failures are not reported
*       through a nonzero return value.
*
****************************************************************************
*
*/

int main (int argc, char **argv)
{
    int minterm;
    for (minterm = 0; minterm < 256; minterm++)
	used_mask[minterm] = generate_expr (minterm);
    find_best_trees ();
    generate_optable ();

    return 0;
}

/****i* januae-gengenblitter/nop *****************************************
*
*   NAME
*       nop() -- Perform no operation.
*
*   SYNOPSIS
*       void nop(int a);
*
*   FUNCTION
*       Ignore the argument without changing any state.
*
*   INPUTS
*       a - Ignored.
*
*   RESULT
*       None.
*
*   NOTES
*       The call site in add_vec() is disabled by conditional compilation.
*
****************************************************************************
*
*/

void nop(int a)
{
}

