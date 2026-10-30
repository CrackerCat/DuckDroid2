// SPDX-License-Identifier: GPL-2.0-only
// Raw-syscall exercise of the device profile: uname, getuid/getgid,
// prctl(PR_GET_NAME), /proc/version, /proc/cpuinfo and /proc/self/status.

#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_READ 63
#define SYS_UNAME 160
#define SYS_PRCTL 167
#define SYS_GETUID 174
#define SYS_GETGID 176
#define AT_FDCWD (-100)
#define PR_GET_NAME 16

static long sys0(long number)
{
	register long x8 __asm__("x8") = number;
	register long x0 __asm__("x0") = 0;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
	return x0;
}

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

static int contains(const char *haystack, long length, const char *needle)
{
	for (long start = 0; start < length; ++start)
	{
		long i = 0;
		while (needle[i] != '\0')
		{
			if (start + i >= length || haystack[start + i] != needle[i])
			{
				break;
			}
			++i;
		}
		if (needle[i] == '\0')
		{
			return 1;
		}
	}
	return 0;
}

static char g_body[4096];

static int expectFileContains(const char *path, const char *needle, int failure)
{
	const long fd = sys4(SYS_OPENAT, AT_FDCWD, (long)path, 0, 0);
	if (fd < 3)
	{
		return failure;
	}
	const long read = sys3(SYS_READ, fd, (long)g_body, sizeof(g_body));
	sys1(SYS_CLOSE, fd);
	if (read <= 0)
	{
		return failure;
	}
	if (!contains(g_body, read, needle))
	{
		return failure;
	}
	return 0;
}

// Returns 0 when every observable matches the injected profile.
int device_checks(void)
{
	char uts[6 * 65];
	if (sys1(SYS_UNAME, (long)uts) != 0)
	{
		return -1;
	}
	if (!contains(uts + 65, 65, "testnode"))
	{
		return -2;
	}
	if (!contains(uts + 2 * 65, 65, "4.9.327-perf-test"))
	{
		return -3;
	}

	if (sys0(SYS_GETUID) != 10042)
	{
		return -4;
	}
	if (sys0(SYS_GETGID) != 20042)
	{
		return -5;
	}

	char name[16];
	if (sys2(SYS_PRCTL, PR_GET_NAME, (long)name) != 0 || !contains(name, 16, "testproc"))
	{
		return -6;
	}

	int failure = expectFileContains("/proc/version", "4.9.327-perf-test", -7);
	if (failure != 0)
	{
		return failure;
	}
	failure = expectFileContains("/proc/cpuinfo", "0x51", -8);
	if (failure != 0)
	{
		return failure;
	}
	failure = expectFileContains("/proc/cpuinfo", "Features", -9);
	if (failure != 0)
	{
		return failure;
	}
	failure = expectFileContains("/proc/self/status", "Uid:\t10042\t10042", -10);
	if (failure != 0)
	{
		return failure;
	}
	failure = expectFileContains("/proc/self/status", "Seccomp:\t2", -11);
	if (failure != 0)
	{
		return failure;
	}
	failure = expectFileContains("/proc/self/status", "Name:\ttestproc", -12);
	if (failure != 0)
	{
		return failure;
	}
	return 0;
}
