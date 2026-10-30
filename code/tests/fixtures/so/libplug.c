// SPDX-License-Identifier: GPL-2.0-only
// Plugin loaded at runtime by 04d_dlopen. The constructor proves dlopen ran
// init_array through the nested guest-call path before it returned.

static int g_constructor_ran;

__attribute__((constructor)) static void plugin_constructor(void)
{
	g_constructor_ran = 42;
}

int plugin_value(void)
{
	return g_constructor_ran;
}
