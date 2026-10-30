// SPDX-License-Identifier: GPL-2.0-only
// Raw-syscall exercise of the descriptor table: dup/dup3/fcntl plus the
// shared-file-description semantics. Built with -nostdlib, so the numbers come
// from the generic arm64 table (asm-generic/unistd.h).

#define SYS_DUP 23
#define SYS_DUP3 24
#define SYS_FCNTL 25
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_LSEEK 62
#define SYS_READ 63
#define SYS_WRITE 64

#define AT_FDCWD (-100)
#define O_RDONLY 0
#define EINVAL 22
#define EBADF 9

#define F_DUPFD 0
#define F_GETFD 1
#define F_GETFL 3

static long sys1(long number, long a)
{
	register long x8 __asm__("x8") = number;
	register long x0 __asm__("x0") = a;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
	return x0;
}

static long sys3(long number, long a, long b, long c)
{
	register long x8 __asm__("x8") = number;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
	return x0;
}

static long sys4(long number, long a, long b, long c, long d)
{
	register long x8 __asm__("x8") = number;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
			 : "memory");
	return x0;
}

static int sameBytes(const char *left, const char *right, int count)
{
	for (int i = 0; i < count; ++i)
	{
		if (left[i] != right[i])
		{
			return 0;
		}
	}
	return 1;
}

int fd_checks(void)
{
	long fd = sys4(SYS_OPENAT, AT_FDCWD, (long)"/proc/self/status", O_RDONLY, 0);
	if (fd < 3)
	{
		return -1;
	}

	long copy = sys1(SYS_DUP, fd);
	if (copy < 3 || copy == fd)
	{
		return -2;
	}

	// A read-only description reports O_RDONLY and no fd flags.
	if (sys3(SYS_FCNTL, fd, F_GETFL, 0) != 0)
	{
		return -3;
	}
	if (sys3(SYS_FCNTL, fd, F_GETFD, 0) != 0)
	{
		return -4;
	}

	char first[32];
	char second[32];
	if (sys3(SYS_READ, fd, (long)first, sizeof(first)) != (long)sizeof(first))
	{
		return -5;
	}
	if (sys3(SYS_READ, copy, (long)second, sizeof(second)) != (long)sizeof(second))
	{
		return -6;
	}
	// Both descriptors share one offset; rewinding the copy rewinds the
	// original, so the next read repeats the first block.
	if (sys3(SYS_LSEEK, copy, 0, 0) != 0)
	{
		return -7;
	}
	if (sys3(SYS_READ, fd, (long)second, sizeof(second)) != (long)sizeof(second))
	{
		return -8;
	}
	if (!sameBytes(first, second, (int)sizeof(first)))
	{
		return -9;
	}

	// dup3 targets an exact descriptor and rejects equal fds or extra flags.
	if (sys3(SYS_DUP3, fd, 42, 0) != 42)
	{
		return -10;
	}
	if (sys3(SYS_DUP3, fd, fd, 0) != -EINVAL)
	{
		return -11;
	}
	if (sys3(SYS_DUP3, fd, 43, 1) != -EINVAL)
	{
		return -12;
	}

	// F_DUPFD honours the minimum descriptor number.
	if (sys3(SYS_FCNTL, fd, F_DUPFD, 100) != 100)
	{
		return -13;
	}

	// Closing one descriptor leaves the description alive for the others.
	if (sys1(SYS_CLOSE, copy) != 0)
	{
		return -14;
	}
	if (sys3(SYS_FCNTL, copy, F_GETFL, 0) != -EBADF)
	{
		return -15;
	}
	if (sys1(SYS_DUP, copy) != -EBADF)
	{
		return -16;
	}
	if (sys3(SYS_READ, fd, (long)first, 4) != 4)
	{
		return -17;
	}

	// dup of stdout keeps writing to the host stream.
	long out = sys1(SYS_DUP, 1);
	if (out < 3)
	{
		return -18;
	}
	static const char message[] = "dup-stdout\n";
	if (sys3(SYS_WRITE, out, (long)message, sizeof(message) - 1) !=
	    (long)sizeof(message) - 1)
	{
		return -19;
	}
	if (sys1(SYS_CLOSE, out) != 0)
	{
		return -20;
	}

	sys1(SYS_CLOSE, 42);
	sys1(SYS_CLOSE, 100);
	sys1(SYS_CLOSE, fd);
	return 0;
}
