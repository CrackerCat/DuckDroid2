// libb.so: the leaf of the dependency chain. Nothing imported.
//
// b_add() touches a module-local counter so the module carries a .data
// relocation and a .bss slot, not just .text.

static int b_calls;

// A self-referential pointer forces an R_AARCH64_RELATIVE in .data. It has to
// be volatile: with a plain `const` pointer LLVM folds the load back to the
// symbol and the relocation disappears (measured 2026-10-28).
static int *volatile b_calls_pointer = &b_calls;

int b_add(int x)
{
	++b_calls;
	return x + 7;
}

int b_calls_made(void)
{
	return *b_calls_pointer;
}
