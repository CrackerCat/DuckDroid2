// SPDX-License-Identifier: GPL-2.0-only
// Drives the virtualized ld-android.so entry points from guest code, matching
// what libc's dlopen()/dlsym() do: they forward here with the caller's return
// address as the last argument.

#define RTLD_NOW 2

extern void *__loader_dlopen(const char *filename, int flags, const void *caller_addr);
extern void *__loader_dlsym(void *handle, const char *symbol, const void *caller_addr);
extern char *__loader_dlerror(void);
extern int __loader_dlclose(void *handle);

static void *g_handle;

int load_plugin(void)
{
	g_handle = __loader_dlopen("libplug.so", RTLD_NOW, __builtin_return_address(0));
	return g_handle != 0 ? 1 : 0;
}

int call_plugin_value(void)
{
	if (g_handle == 0)
	{
		return -1;
	}
	int (*fn)(void) = (int (*)(void))__loader_dlsym(g_handle,
		"plugin_value",
		__builtin_return_address(0));
	if (fn == 0)
	{
		return -2;
	}
	return fn();
}

// 1: the symbol lookup failed and left a dlerror message behind.
int lookup_missing(void)
{
	if (g_handle == 0)
	{
		return -1;
	}
	void *symbol = __loader_dlsym(g_handle, "plugin_missing", __builtin_return_address(0));
	if (symbol != 0)
	{
		return 0;
	}
	return __loader_dlerror() != 0 ? 1 : 0;
}

// 1: a failed dlopen returns NULL, dlerror reports it once and is then clear.
int open_missing(void)
{
	void *handle = __loader_dlopen("libnope.so", RTLD_NOW, __builtin_return_address(0));
	if (handle != 0)
	{
		return -1;
	}
	if (__loader_dlerror() == 0)
	{
		return -2;
	}
	if (__loader_dlerror() != 0)
	{
		return -3;
	}
	return 1;
}

// 1: reopening an already loaded library yields the same handle, and dlclose
// does not unload it (the next call_plugin_value still works).
int reload_same_handle(void)
{
	void *again = __loader_dlopen("libplug.so", RTLD_NOW, __builtin_return_address(0));
	if (again == 0 || again != g_handle)
	{
		return 0;
	}
	__loader_dlclose(again);
	return 1;
}
