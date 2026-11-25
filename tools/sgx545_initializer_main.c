// SPDX-License-Identifier: GPL-2.0-only
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	int selftest = argc == 2 && !strcmp(argv[1], "--selftest");
	void *handle;
	int (*initialize)(void);
	const char *(*version)(void);
	int result;
	if (argc > 2 || (argc == 2 && !selftest)) return 2;
	handle = dlopen(selftest ? "libsgx545_initializer_fixture.so" : "libsrv_init.so", RTLD_NOW | RTLD_LOCAL);
	if (!handle) { fprintf(stderr, "LOADER error=%s\n", dlerror()); return 2; }
	version = (const char *(*)(void))dlsym(handle, "gma3600_sgx_transport_version");
	initialize = (int (*)(void))dlsym(handle, "SrvInit");
	if (!initialize || !version || strcmp(version(), "0.1.3")) {
		fprintf(stderr, "LOADER missing initializer or private transport\n");
		dlclose(handle);
		return 2;
	}
	fprintf(stderr, "LOADER mode=%s transport=%s\n", selftest ? "open-source-fixture" : "Intel-initializer", version());
	result = initialize();
	fprintf(stderr, "INITIALIZER return=%d gpu_execution=disabled\n", result);
	dlclose(handle);
	if (selftest) return result ? 1 : 0;
	/* A normal initializer cannot succeed through the prepare-only bridge. */
	return result ? 3 : 4;
}
