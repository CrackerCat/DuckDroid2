// SPDX-License-Identifier: GPL-2.0-only
// Provides the link-time definitions so liba.so records DT_NEEDED libc.so and
// keeps its imports UND. The file is never shipped to the guest search path:
// the loader must satisfy these symbols from the virtual bionic table.

int *__errno(void)
{
	static int value;
	return &value;
}

long write(int fd, const void *buffer, unsigned long count)
{
	(void)fd;
	(void)buffer;
	return (long)count;
}

void *malloc(unsigned long size)
{
	(void)size;
	return 0;
}

void free(void *pointer)
{
	(void)pointer;
}

int pthread_once(int *once_control, void (*init_routine)(void))
{
	(void)once_control;
	(void)init_routine;
	return 0;
}
