// SPDX-License-Identifier: GPL-2.0-only
// Dynamic TLS / DTV fixture.
//
// The module has one __thread variable and is loaded after the real libc has
// finalized static TLS, so its TLSDESC descriptor points at the dynamic
// resolver and its storage lives in each thread's DTV. The helpers create
// bionic threads through the real libc and can park one while the host loads
// another TLS module.
//
// The same source is built twice: libtlsdtv.so (dynamic; loaded after libc)
// and libtlsdtv_static.so (DT_NEEDED libc.so; loaded before libc so it gets a
// static slot). The build difference is only the DT_NEEDED entry.

#define SYS_FUTEX 98
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define RTLD_NOW 2
#define RTLD_DEFAULT ((void *)0)

extern int pthread_create(unsigned long *thread, const void *attr,
	void *(*start)(void *), void *arg);
extern int pthread_join(unsigned long thread, void **retval);
extern void *dlopen(const char *filename, int flags);
extern void *dlsym(void *handle, const char *symbol);

__thread int g_dtv_tls = 7;

int dtv_get(void)
{
	return g_dtv_tls;
}

int dtv_set(int value)
{
	g_dtv_tls = value;
	return 0;
}

// dlsym of an STT_TLS symbol must return this thread's copy, not a module
// address. RTLD_DEFAULT searches the global scope, so the same code works for
// the dynamic and the DT_NEEDED-static build of this fixture.
static void *g_dlsym_address;

int dtv_dlsym_addr(void)
{
	g_dlsym_address = dlsym(RTLD_DEFAULT, "g_dtv_tls");
	return g_dlsym_address != 0 ? 0 : -1;
}

int dtv_dlsym_get(void)
{
	return g_dlsym_address != 0 ? *(volatile int *)g_dlsym_address : -1;
}

int dtv_dlsym_set(int value)
{
	if (g_dlsym_address == 0)
	{
		return -1;
	}
	*(volatile int *)g_dlsym_address = value;
	return 0;
}

static long futex(volatile int *address, int op, int value)
{
	register long x8 __asm__("x8") = SYS_FUTEX;
	register long x0 __asm__("x0") = (long)address;
	register long x1 __asm__("x1") = op;
	register long x2 __asm__("x2") = value;
	register long x3 __asm__("x3") = 0;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

// A bionic-created thread must see the module initializer (7) and get its own
// copy (9). Returns 7009 when both hold.
static volatile int g_seen_before;
static volatile int g_seen_after;

static void *copy_worker(void *arg)
{
	(void)arg;
	g_seen_before = g_dtv_tls;
	g_dtv_tls = 9;
	g_seen_after = g_dtv_tls;
	return 0;
}

int dtv_thread_probe(void)
{
	unsigned long thread = 0;
	if (pthread_create(&thread, 0, copy_worker, 0) != 0)
	{
		return -1;
	}
	if (pthread_join(thread, 0) != 0)
	{
		return -2;
	}
	return (int)g_seen_before * 1000 + (int)g_seen_after;
}

// Park a thread, let the host load another TLS module, then access that module
// from the parked thread. The DTV generation is stale, so the first access
// must take __tls_get_addr's slow path.
static volatile int *g_gate;
static unsigned long g_worker_thread;
static volatile int g_worker_state; // 1 = parked, 2 = finished
static volatile int g_loaded_tls;

static void *gate_worker(void *arg)
{
	(void)arg;
	g_worker_state = 1;
	while (*g_gate == 0)
	{
		futex(g_gate, FUTEX_WAIT, 0);
	}
	void *handle = dlopen("libtls.so", RTLD_NOW);
	int (*tls_get)(void) = (int (*)(void))dlsym(handle, "tls_get");
	g_loaded_tls = tls_get != 0 ? tls_get() : -1;
	g_worker_state = 2;
	return 0;
}

int start_gate_worker(volatile int *gate)
{
	g_gate = gate;
	g_worker_state = 0;
	g_loaded_tls = 0;
	return pthread_create(&g_worker_thread, 0, gate_worker, 0);
}

int gate_worker_state(void)
{
	return g_worker_state;
}

int gate_worker_tls(void)
{
	return g_loaded_tls;
}

int finish_gate_worker(volatile int *gate)
{
	*gate = 1;
	futex(gate, FUTEX_WAKE, 1);
	return pthread_join(g_worker_thread, 0);
}
