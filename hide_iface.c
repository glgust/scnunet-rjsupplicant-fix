#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Force dlsym to use GLIBC_2.2.5 compat symbol for Ubuntu 20.04 (glibc 2.31) compatibility */
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");
#include <dlfcn.h>
#include <ifaddrs.h>

typedef int  (*real_getifaddrs_fn)(struct ifaddrs **);
typedef void (*real_freeifaddrs_fn)(struct ifaddrs *);

static const char *hidden_ifaces[16];
static int hidden_count = 0;

__attribute__((constructor))
static void init(void) {
    const char *env = getenv("HIDE_IFACE");
    if (!env) return;
    char *s = strdup(env);
    char *tok = strtok(s, ",");
    while (tok && hidden_count < 16) {
        while (*tok == ' ') tok++;
        hidden_ifaces[hidden_count++] = tok;
        tok = strtok(NULL, ",");
    }
}

static int should_hide(const char *name) {
    for (int i = 0; i < hidden_count; i++) {
        if (strcmp(name, hidden_ifaces[i]) == 0) return 1;
    }
    return 0;
}

int getifaddrs(struct ifaddrs **ifap) {
    real_getifaddrs_fn real_fn;
    real_fn = (real_getifaddrs_fn)dlsym(RTLD_NEXT, "getifaddrs");
    if (!real_fn) return -1;

    struct ifaddrs *head;
    int ret = real_fn(&head);
    if (ret != 0) return ret;

    /* 从链表中过滤掉隐藏的网卡 */
    struct ifaddrs *prev = NULL;
    struct ifaddrs *ifa = head;
    while (ifa) {
        if (should_hide(ifa->ifa_name)) {
            struct ifaddrs *next = ifa->ifa_next;
            if (prev)
                prev->ifa_next = next;
            else
                head = next;
            ifa = next;
        } else {
            prev = ifa;
            ifa = ifa->ifa_next;
        }
    }

    *ifap = head;
    return 0;
}

void freeifaddrs(struct ifaddrs *ifa) {
    real_freeifaddrs_fn real_fn;
    real_fn = (real_freeifaddrs_fn)dlsym(RTLD_NEXT, "freeifaddrs");
    if (real_fn)
        real_fn(ifa);
}
