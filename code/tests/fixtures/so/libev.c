// SPDX-License-Identifier: GPL-2.0-only
// Raw-syscall exercise of the event loop: eventfd2, ppoll, epoll_create1/
// epoll_ctl/epoll_pwait and timerfd_create/settime, including a blocking wait
// that only a timer can end.

#define SYS_EVENTFD2 19
#define SYS_EPOLL_CREATE1 20
#define SYS_EPOLL_CTL 21
#define SYS_EPOLL_PWAIT 22
#define SYS_CLOSE 57
#define SYS_PIPE2 59
#define SYS_READ 63
#define SYS_WRITE 64
#define SYS_PPOLL 73
#define SYS_TIMERFD_CREATE 85
#define SYS_TIMERFD_SETTIME 86

#define POLLIN 0x001
#define EPOLL_CTL_ADD 1

struct pollfd
{
	int fd;
	short events;
	short revents;
};

struct timespec
{
	long seconds;
	long nanoseconds;
};

struct itimerspec
{
	struct timespec interval;
	struct timespec value;
};

struct epoll_event
{
	unsigned int events;
	unsigned long long data;
} __attribute__((packed));

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

static long sys5(long number, long a, long b, long c, long d, long e)
{
	register long x8 __asm__("x8") = number;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
			 : "memory");
	return x0;
}

static long sys6(long number, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = number;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory");
	return x0;
}

int event_checks(void)
{
	// ------------------------------------------------------------------
	// eventfd + poll, immediate readiness and drain.
	// ------------------------------------------------------------------
	long event = sys2(SYS_EVENTFD2, 0, 0);
	if (event < 3)
	{
		return -1;
	}
	unsigned long long one = 1;
	if (sys3(SYS_WRITE, event, (long)&one, 8) != 8)
	{
		return -2;
	}

	struct pollfd single;
	single.fd = (int)event;
	single.events = POLLIN;
	single.revents = 0;
	struct timespec zero;
	zero.seconds = 0;
	zero.nanoseconds = 0;
	if (sys5(SYS_PPOLL, (long)&single, 1, (long)&zero, 0, 0) != 1)
	{
		return -3;
	}
	if ((single.revents & POLLIN) == 0)
	{
		return -4;
	}
	unsigned long long value = 0;
	if (sys3(SYS_READ, event, (long)&value, 8) != 8 || value != 1)
	{
		return -5;
	}
	single.revents = 0;
	if (sys5(SYS_PPOLL, (long)&single, 1, (long)&zero, 0, 0) != 0)
	{
		return -6;
	}

	// ------------------------------------------------------------------
	// epoll: registration, immediate readiness, data passthrough.
	// ------------------------------------------------------------------
	long epoll = sys1(SYS_EPOLL_CREATE1, 0);
	if (epoll < 3)
	{
		return -7;
	}
	struct epoll_event interest;
	interest.events = POLLIN;
	interest.data = 0x1234;
	if (sys4(SYS_EPOLL_CTL, epoll, EPOLL_CTL_ADD, event, (long)&interest) != 0)
	{
		return -8;
	}
	if (sys3(SYS_WRITE, event, (long)&one, 8) != 8)
	{
		return -9;
	}
	struct epoll_event ready;
	ready.events = 0;
	ready.data = 0;
	if (sys6(SYS_EPOLL_PWAIT, epoll, (long)&ready, 8, 0, 0, 0) != 1)
	{
		return -10;
	}
	if ((ready.events & POLLIN) == 0 || ready.data != 0x1234)
	{
		return -11;
	}
	sys3(SYS_READ, event, (long)&value, 8);

	// ------------------------------------------------------------------
	// timerfd ending a 1 s poll: the wait must wake on the 10 ms timer.
	// ------------------------------------------------------------------
	long timer = sys2(SYS_TIMERFD_CREATE, 1, 0);
	if (timer < 3)
	{
		return -12;
	}
	struct itimerspec spec;
	spec.interval.seconds = 0;
	spec.interval.nanoseconds = 0;
	spec.value.seconds = 0;
	spec.value.nanoseconds = 10000000;
	if (sys4(SYS_TIMERFD_SETTIME, timer, 0, (long)&spec, 0) != 0)
	{
		return -13;
	}
	struct pollfd timed;
	timed.fd = (int)timer;
	timed.events = POLLIN;
	timed.revents = 0;
	struct timespec second;
	second.seconds = 1;
	second.nanoseconds = 0;
	if (sys5(SYS_PPOLL, (long)&timed, 1, (long)&second, 0, 0) != 1)
	{
		return -14;
	}
	if ((timed.revents & POLLIN) == 0)
	{
		return -15;
	}
	unsigned long long expirations = 0;
	if (sys3(SYS_READ, timer, (long)&expirations, 8) != 8 || expirations != 1)
	{
		return -16;
	}

	// ------------------------------------------------------------------
	// The same timer inside an infinite epoll wait.
	// ------------------------------------------------------------------
	if (sys4(SYS_EPOLL_CTL, epoll, EPOLL_CTL_ADD, timer, (long)&interest) != 0)
	{
		return -17;
	}
	spec.value.nanoseconds = 5000000;
	if (sys4(SYS_TIMERFD_SETTIME, timer, 0, (long)&spec, 0) != 0)
	{
		return -18;
	}
	if (sys6(SYS_EPOLL_PWAIT, epoll, (long)&ready, 8, -1, 0, 0) != 1)
	{
		return -19;
	}
	sys3(SYS_READ, timer, (long)&expirations, 8);

	// ------------------------------------------------------------------
	// Self-pipe: write, then poll the read end.
	// ------------------------------------------------------------------
	int pipe_fds[2];
	pipe_fds[0] = -1;
	pipe_fds[1] = -1;
	if (sys2(SYS_PIPE2, (long)pipe_fds, 0) != 0)
	{
		return -20;
	}
	if (pipe_fds[0] < 3 || pipe_fds[1] < 3)
	{
		return -21;
	}
	struct pollfd pipe_poll;
	pipe_poll.fd = pipe_fds[0];
	pipe_poll.events = POLLIN;
	pipe_poll.revents = 0;
	if (sys5(SYS_PPOLL, (long)&pipe_poll, 1, (long)&zero, 0, 0) != 0)
	{
		return -22;
	}
	if (sys3(SYS_WRITE, pipe_fds[1], (long)"ab", 2) != 2)
	{
		return -23;
	}
	pipe_poll.revents = 0;
	if (sys5(SYS_PPOLL, (long)&pipe_poll, 1, (long)&zero, 0, 0) != 1)
	{
		return -24;
	}
	if ((pipe_poll.revents & POLLIN) == 0)
	{
		return -25;
	}
	char pipe_bytes[2];
	if (sys3(SYS_READ, pipe_fds[0], (long)pipe_bytes, 2) != 2)
	{
		return -26;
	}
	if (pipe_bytes[0] != 'a' || pipe_bytes[1] != 'b')
	{
		return -27;
	}
	struct pollfd write_poll;
	write_poll.fd = pipe_fds[1];
	write_poll.events = 0x004;
	write_poll.revents = 0;
	if (sys5(SYS_PPOLL, (long)&write_poll, 1, (long)&zero, 0, 0) != 1)
	{
		return -28;
	}

	sys1(SYS_CLOSE, event);
	sys1(SYS_CLOSE, timer);
	sys1(SYS_CLOSE, epoll);
	sys1(SYS_CLOSE, pipe_fds[0]);
	sys1(SYS_CLOSE, pipe_fds[1]);
	return 0;
}
