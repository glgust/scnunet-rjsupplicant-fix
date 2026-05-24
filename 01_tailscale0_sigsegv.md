# Bug: rjsupplicant SIGSEGV on Linux with tailscale0 (link/none) interface

## Summary

rjsupplicant (2014 binary) crashes with SIGSEGV when a `link/none` type virtual network interface (e.g. Tailscale's `tailscale0`) exists on the system. The crash occurs at `get_nics_info+0xc9` (offset `0x451999`), dereferencing a NULL `ifa_addr` pointer.

## Affected Versions

- rjsupplicant: 2014 binary (`ELF 64-bit LSB executable, x86-64, for GNU/Linux 2.6.9`)
- Reproduced on: Ubuntu 24.04.4 LTS, kernel 6.8.0-100-generic
- Any Linux with `link/none` virtual interfaces (Tailscale, WireGuard TUN, etc.)

## Reproduction

### Prerequisites

- rjsupplicant installed and configured
- Tailscale (or any software creating a `link/none` TUN interface) installed
- The rjsupplicant process must be able to see the `link/none` interface via `getifaddrs()`

### Steps

```bash
# 1. Verify tailscale0 exists with link/none type
ip -d link show tailscale0
# Output: link/none ... type tun

# 2. Ensure HIDE_IFACE does NOT hide tailscale0
grep HIDE_IFACE /opt/rjsupplicant/x64/run.sh
# HIDE_IFACE=lan0  (tailscale0 NOT in list)

# 3. Clear kernel log buffer
sudo dmesg -c > /dev/null

# 4. Start rjsupplicant
sudo systemctl start rjsupplicant

# 5. Wait 3 seconds, check crash log
sleep 3
sudo dmesg | grep segfault
```

### Expected Result

```
[40852.002827] rjsupplicant[132540]: segfault at 0 ip 0000000000451999 \
  sp 00007292fab348f0 error 4 in rjsupplicant[400000+10a000] likely on CPU 1
```

rjsupplicant crashes in an infinite restart loop (systemd `Restart=always` → crash → restart → crash).

## Root Cause

rjsupplicant (compiled 2014, `GNU/Linux 2.6.9`) iterates over all network interfaces via `getifaddrs()`. In `get_nics_info()`, the code assumes that **any non-loopback interface has a MAC address** and directly dereferences `ifa_addr` without a NULL check.

In 2014 Linux, this assumption was valid — every interface was either loopback or had a physical MAC. Modern Linux introduces `link/none` TUN interfaces (Tailscale `tailscale0`, WireGuard `wg0`, etc.), which have no link-layer address. For these interfaces, `ifa_addr` is NULL in `getifaddrs()` output.

### Disassembly Evidence

```asm
; get_nics_info+0x80 at 0x451950, crash at +0xc9 = 0x451999

0x451990:  41 f6 45 10 08      testb  $0x8, 0x10(%r13)   ; Check IFF_LOOPBACK flag
0x451995:  49 8b 45 18         mov    0x18(%r13), %rax   ; Load ifa_addr pointer
0x451999:  44 0f b7 30         movzwl (%rax), %r14d      ; ★ CRASH: deref NULL RAX
```

### struct ifaddrs Layout (x86-64)

```
Offset  Field         Purpose
+0x00   ifa_next       Next interface in linked list (8 bytes)
+0x08   ifa_name       Interface name string pointer (8 bytes)
+0x10   ifa_flags      Interface flags, IFF_LOOPBACK = 0x8 (4 bytes, padding)
+0x18   ifa_addr       Link-layer address (sockaddr*) ← NULL for link/none
+0x20   ifa_netmask    Netmask
...
```

### Crash Flow

```
get_nics_info() iterates getifaddrs() linked list
  → Encounters tailscale0 (link/none)
  → testb $0x8, +0x10: IFF_LOOPBACK NOT set → passes bypass check
  → mov +0x18, %rax: loads ifa_addr → RAX = 0x0 (NULL)
  → movzwl (%rax), %r14d: dereference NULL → SIGSEGV
```

### Interface Properties

```
$ ip -d link show tailscale0
tailscale0: <POINTOPOINT,MULTICAST,NOARP,UP,LOWER_UP> mtu 1280 ...
    link/none ...
    tun type tun ...
```

`link/none` = no link-layer address → `ifa_addr` = NULL.

## Fix

### Approach: LD_PRELOAD + getifaddrs() interposition

A small shared library (`hide_iface.so`) intercepts `getifaddrs()`, scans the returned linked list, and removes interfaces listed in the `HIDE_IFACE` environment variable before passing the result to the caller.

### hide_iface.c (72 lines)

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");  /* glibc 2.31 compat */
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
    for (int i = 0; i < hidden_count; i++)
        if (strcmp(name, hidden_ifaces[i]) == 0) return 1;
    return 0;
}

int getifaddrs(struct ifaddrs **ifap) {
    real_getifaddrs_fn real_fn;
    real_fn = (real_getifaddrs_fn)dlsym(RTLD_NEXT, "getifaddrs");
    if (!real_fn) return -1;

    struct ifaddrs *head;
    int ret = real_fn(&head);
    if (ret != 0) return ret;

    struct ifaddrs *prev = NULL, *ifa = head;
    while (ifa) {
        if (should_hide(ifa->ifa_name)) {
            struct ifaddrs *next = ifa->ifa_next;
            if (prev) prev->ifa_next = next;
            else      head = next;
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
    if (real_fn) real_fn(ifa);
}
```

### Deployment

```bash
# 1. Compile
gcc -shared -fPIC -o hide_iface.so hide_iface.c -ldl

# 2. Install
sudo cp hide_iface.so /usr/local/lib/

# 3. Preload system-wide (via /etc/ld.so.preload)
echo "/usr/local/lib/hide_iface.so" | sudo tee /etc/ld.so.preload

# 4. Configure rjsupplicant
# In /opt/rjsupplicant/x64/run.sh:
export HIDE_IFACE=tailscale0,enp1s0
```

### rjsupplicant's view (before → after)

```
Before fix:                          After fix (HIDE_IFACE=tailscale0,enp1s0):
┌──────────────────┐                ┌──────────────┐
│ lo               │                │ lo           │
│ enp1s0 (LAN)     │ ← 802.1X检测  │ enp2s0 (WAN) │ ← 仅剩认证网卡
│ enp2s0 (WAN)     │    到多网卡   │              │
│ tailscale0       │ ← NULL MAC →  └──────────────┘
│   link/none      │    SIGSEGV
└──────────────────┘
```

## Verification

### Crash Reproduced (2026-05-25)

```
Environment: Ubuntu 24.04.4 LTS, kernel 6.8.0-100-generic
Tailscale: v1.98.3, tailscale0 type link/none
rjsupplicant: 2014 binary, ELF64

$ sudo dmesg -c > /dev/null
$ sudo systemctl start rjsupplicant    # HIDE_IFACE=enp1s0 (no tailscale0)
$ sleep 3
$ sudo dmesg | grep segfault

[40852.002827] rjsupplicant[132540]: segfault at 0 ip 0000000000451999 \
  sp 00007292fab348f0 error 4 in rjsupplicant[400000+10a000] likely on CPU 1
```

### Fix Verified (2026-05-25)

```
$ grep HIDE_IFACE /opt/rjsupplicant/x64/run.sh
export HIDE_IFACE=tailscale0,enp1s0

$ sudo systemctl restart rjsupplicant
$ sleep 3
$ systemctl is-active rjsupplicant
active

$ sudo dmesg | grep segfault | wc -l
1    ← only the crash from the reproduction above, no new ones
```

## Key Files

| File | Path |
|------|------|
| Crash binary | `/opt/rjsupplicant/x64/rjsupplicant` (2014, ELF64, not stripped) |
| Fix source | `hide_iface.c` |
| Fix binary | `/usr/local/lib/hide_iface.so` |
| Preload config | `/etc/ld.so.preload` |
| rjsupplicant launcher | `/opt/rjsupplicant/x64/run.sh` |
| systemd unit | `/etc/systemd/system/rjsupplicant.service` |

## Related

- Linux kernel `link/none`: introduced for TUN/TAP interfaces without link-layer addressing
- rjsupplicant version: 2014, targeting `GNU/Linux 2.6.9` (kernel ~2004 era, pre-TUN `link/none`)
- This is a **forward-compatibility bug**: code written for 2004-era kernel assumptions breaks on modern kernel interfaces
