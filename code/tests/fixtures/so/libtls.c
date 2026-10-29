// SPDX-License-Identifier: GPL-2.0-only
// M4.5 TLSDESC fixture. clang's aarch64-linux-android target emits
// R_AARCH64_TLSDESC for __thread access (there is no -mtls-dialect=trad), so
// this exercises the descriptor + resolver-stub path against a static TLS
// slot assigned by the loader.

__thread int g_tls = 7;

int tls_get(void)
{
	return g_tls;
}

void tls_set(int value)
{
	g_tls = value;
}
