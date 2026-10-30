// SPDX-License-Identifier: GPL-2.0-only
// Raw-syscall exercise of the writable sandbox: open with O_CREAT/O_TRUNC/
// O_APPEND, write, fsync, ftruncate, stat, mkdirat, unlinkat, renameat.

#define SYS_MKDIRAT 34
#define SYS_UNLINKAT 35
#define SYS_RENAMEAT 38
#define SYS_FTRUNCATE 46
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_LSEEK 62
#define SYS_READ 63
#define SYS_WRITE 64
#define SYS_NEWFSTATAT 79
#define SYS_FSYNC 82

#define AT_FDCWD (-100)
#define AT_REMOVEDIR 0x200
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x40
#define O_TRUNC 0x200
#define O_APPEND 0x400

#define ENOENT 2
#define EISDIR 21
#define EROFS 30

static long sys1(long number, long a)
{
	register long x8 __asm__("x8") = number;
	register long x0 __asm__("x0") = a;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
	return x0;
}

static long sys2(long number, long a, long b)
{
	register long x8 __asm__("x8") = number;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
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

static unsigned long long readU64(const unsigned char *bytes, int offset)
{
	unsigned long long value = 0;
	for (int i = 0; i < 8; ++i)
	{
		value |= (unsigned long long)bytes[offset + i] << (8 * i);
	}
	return value;
}

static unsigned readU32(const unsigned char *bytes, int offset)
{
	unsigned value = 0;
	for (int i = 0; i < 4; ++i)
	{
		value |= (unsigned)bytes[offset + i] << (8 * i);
	}
	return value;
}

// Returns 0 when the whole write path behaves; each negative value names the
// failing step.
int write_checks(void)
{
	// A file the host placed in the sandbox is visible through the overlay.
	long seed = sys4(SYS_OPENAT, AT_FDCWD, (long)"/data/preloaded.txt", O_RDONLY, 0);
	if (seed < 3)
	{
		return -1;
	}
	char buffer[16];
	if (sys3(SYS_READ, seed, (long)buffer, 4) != 4)
	{
		return -2;
	}
	if (!sameBytes(buffer, "seed", 4))
	{
		return -3;
	}
	sys1(SYS_CLOSE, seed);

	long fd = sys4(SYS_OPENAT, AT_FDCWD,
		(long)"/data/local/tmp/wr.txt", O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 3)
	{
		return -4;
	}
	if (sys3(SYS_WRITE, fd, (long)"hello", 5) != 5)
	{
		return -5;
	}
	if (sys1(SYS_FSYNC, fd) != 0)
	{
		return -6;
	}
	if (sys3(SYS_LSEEK, fd, 0, 0) != 0)
	{
		return -7;
	}
	if (sys3(SYS_READ, fd, (long)buffer, 5) != 5)
	{
		return -8;
	}
	if (!sameBytes(buffer, "hello", 5))
	{
		return -9;
	}
	if (sys2(SYS_FTRUNCATE, fd, 2) != 0)
	{
		return -10;
	}
	if (sys3(SYS_LSEEK, fd, 0, 2) != 2)
	{
		return -11;
	}
	if (sys1(SYS_CLOSE, fd) != 0)
	{
		return -12;
	}

	// O_APPEND writes land at the end regardless of the descriptor offset.
	long append = sys4(SYS_OPENAT, AT_FDCWD,
		(long)"/data/local/tmp/wr.txt", O_WRONLY | O_APPEND, 0);
	if (append < 3)
	{
		return -13;
	}
	if (sys3(SYS_WRITE, append, (long)"XY", 2) != 2)
	{
		return -14;
	}
	sys1(SYS_CLOSE, append);

	unsigned char stat_bytes[128];
	for (int i = 0; i < 128; ++i)
	{
		stat_bytes[i] = 0;
	}
	if (sys4(SYS_NEWFSTATAT, AT_FDCWD,
		     (long)"/data/local/tmp/wr.txt", (long)stat_bytes, 0) != 0)
	{
		return -15;
	}
	if (readU64(stat_bytes, 48) != 4)
	{
		return -16;
	}
	// The owner is the configured app uid/gid, not the host user.
	if (readU32(stat_bytes, 24) != 1000 || readU32(stat_bytes, 28) != 1000)
	{
		return -17;
	}

	if (sys4(SYS_RENAMEAT, AT_FDCWD,
		     (long)"/data/local/tmp/wr.txt", AT_FDCWD,
		     (long)"/data/local/tmp/wr2.txt") != 0)
	{
		return -18;
	}
	if (sys4(SYS_OPENAT, AT_FDCWD, (long)"/data/local/tmp/wr.txt", O_RDONLY, 0) != -ENOENT)
	{
		return -19;
	}

	if (sys3(SYS_MKDIRAT, AT_FDCWD, (long)"/data/local/tmp/wdir", 0700) != 0)
	{
		return -20;
	}
	// unlinkat without AT_REMOVEDIR refuses a directory.
	if (sys3(SYS_UNLINKAT, AT_FDCWD, (long)"/data/local/tmp/wdir", 0) != -EISDIR)
	{
		return -21;
	}
	if (sys3(SYS_UNLINKAT, AT_FDCWD, (long)"/data/local/tmp/wdir", AT_REMOVEDIR) != 0)
	{
		return -22;
	}
	if (sys3(SYS_UNLINKAT, AT_FDCWD, (long)"/data/local/tmp/wr2.txt", 0) != 0)
	{
		return -23;
	}
	if (sys4(SYS_OPENAT, AT_FDCWD, (long)"/data/local/tmp/wr2.txt", O_RDONLY, 0) != -ENOENT)
	{
		return -24;
	}
	return 0;
}

// Without a writable sandbox every mutating call answers EROFS.
int write_refused(void)
{
	long fd = sys4(SYS_OPENAT, AT_FDCWD, (long)"/data/x.txt", O_RDWR | O_CREAT, 0600);
	if (fd != -EROFS)
	{
		return 1;
	}
	if (sys3(SYS_MKDIRAT, AT_FDCWD, (long)"/data/dir", 0700) != -EROFS)
	{
		return 2;
	}
	if (sys3(SYS_UNLINKAT, AT_FDCWD, (long)"/data/x.txt", 0) != -EROFS)
	{
		return 3;
	}
	return 0;
}
