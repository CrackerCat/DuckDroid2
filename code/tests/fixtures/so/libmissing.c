// Loads fine, but references an object symbol nobody provides. The loader must
// refuse the load with the symbol name in the error.

extern int nobody_provides_this_object;

int libmissing_entry(void)
{
	return nobody_provides_this_object;
}
