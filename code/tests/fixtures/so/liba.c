// liba.so: NEEDED libb.so + liblog.so, and three undefined functions that must
// be satisfied by the virtual bionic (write / pthread_once) and by the virtual
// liblog module (log_print).
//
// The constructor proves DT_INIT_ARRAY runs at load time.

extern int b_add(int);
extern int log_print(int);
extern long write(int fd, const void *buffer, unsigned long count);
extern int pthread_once(int *once_control, void (*init_routine)(void));
extern void *malloc(unsigned long size);
extern void free(void *pointer);
extern int *__errno(void);

int g_ctor_ran;
int g_once_ran;

static void once_body(void)
{
	g_once_ran += 1;
}

int a_entry(int x)
{
	static const char message[] = "liba\n";

	static int once_control;
	if (pthread_once(&once_control, once_body) != 0) {
		return -1;
	}

	if (log_print(x) == 0) {
		return -2;
	}

	if (__errno() == 0) {
		return -4;
	}

	// volatile keeps the store/load pair from being folded away.
	volatile int *block = (volatile int *)malloc(16);
	if (block == 0) {
		return -5;
	}
	*block = x;
	const int stored = *block;
	free((void *)block);
	if (stored != x) {
		return -6;
	}

	write(1, message, sizeof(message) - 1);
	return b_add(x) + g_once_ran;
}

// The body must have an effect the optimizer cannot fold into .data, or
// clang -O1 drops the .init_array entry entirely (measured 2026-10-28).
__attribute__((constructor)) static void a_ctor(void)
{
	g_ctor_ran = b_add(1);
}
