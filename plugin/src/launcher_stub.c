/*
 * launcher_stub.c
 *
 * Statically-linked launcher that clears any inherited LD_PRELOAD /
 * LD_LIBRARY_PATH before exec'ing the real (dynamically-linked) emulator
 * binary, shipped alongside it as "emu7800.real".
 *
 * Some webOS TLS/OpenSSL upgrade patches (e.g. OpenSSL-legacyWebOS) add
 * LD_PRELOAD/LD_LIBRARY_PATH to LunaSysMgr's upstart job environment to
 * route apps through a private OpenSSL 1.1.1 stack under /usr/lib/ssl11.
 * Those variables are inherited by every app LunaSysMgr launches, including
 * PDK apps. /usr/lib/ssl11 is not exposed inside the PDK app jail
 * (/etc/jail_pdk.conf), so the dynamic linker fails to resolve the
 * preloaded shim before main() ever runs, and the real emulator binary
 * silently fails to launch -- no window, no crash dialog, nothing.
 *
 * A statically-linked binary has no dynamic-linker pass at all, so
 * LD_PRELOAD/LD_LIBRARY_PATH cannot affect it. This is appinfo.json's
 * "main" binary; unsetting the two variables here and re-exec'ing the real
 * binary gives it a clean environment regardless of what the parent
 * process leaked in. A no-op when nothing was leaked (stock webOS,
 * TouchPad), so this is safe to ship everywhere.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>

int main(int argc, char *argv[])
{
    char self[PATH_MAX];
    char real_path[PATH_MAX];
    char *dir;
    ssize_t n;

    unsetenv("LD_PRELOAD");
    unsetenv("LD_LIBRARY_PATH");

    /* Resolve our own directory so this works regardless of CWD */
    n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        dir = dirname(self);
    } else if (argc > 0 && argv[0][0] != '\0') {
        strncpy(self, argv[0], sizeof(self) - 1);
        self[sizeof(self) - 1] = '\0';
        dir = dirname(self);
    } else {
        dir = ".";
    }

    snprintf(real_path, sizeof(real_path), "%s/emu7800.real", dir);
    execv(real_path, argv);

    /* If we get here, exec failed -- nothing we can do without a UI. */
    return 1;
}
