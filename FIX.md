# 修复方案

## 两种路线

| 路线 | 适用场景 | 难度 |
|------|----------|:--:|
| **A. LD_PRELOAD 拦截**（推荐） | 不想动 rjsupplicant，一劳永逸 | 低 |
| **B. 直接停止虚拟网卡** | 临时测试、不常用 Tailscale/WireGuard | 最低 |

---

## 路线 A：LD_PRELOAD 拦截 `getifaddrs()`

### 原理

编译一个极小的共享库（`.so`），注入到 rjsupplicant 启动时的库加载顺序中。拦截 `getifaddrs()` 调用，在返回网卡链表给 rjsupplicant 之前，把有问题的接口从链表里删掉。

rjsupplicant 不需要修改、不需要重新编译，运行环境完全透明。

### 步骤 1：编写 `hide_iface.c`

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* dlsym@GLIBC_2.2.5 兼容符号，确保旧 glibc 系统可用 */
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");
#include <dlfcn.h>
#include <ifaddrs.h>

typedef int  (*real_getifaddrs_fn)(struct ifaddrs **);
typedef void (*real_freeifaddrs_fn)(struct ifaddrs *);

static const char *hidden_ifaces[16];
static int hidden_count = 0;

/* 构造函数：程序启动时自动执行，读取 HIDE_IFACE 环境变量 */
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

/* 拦截 getifaddrs()，从链表删除隐藏列表中的接口 */
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

完整源码：仓库根目录 [`hide_iface.c`](hide_iface.c)。

### 步骤 2：编译

```bash
gcc -shared -fPIC -o hide_iface.so hide_iface.c -ldl
```

编译后约 10KB，干净无依赖。

### 步骤 3：部署

```bash
# 安装 .so 到系统库目录
sudo cp hide_iface.so /usr/local/lib/

# 全局预加载（写入 /etc/ld.so.preload）
echo "/usr/local/lib/hide_iface.so" | sudo tee /etc/ld.so.preload
```

> **注意：** `/etc/ld.so.preload` 对系统上所有进程生效。`hide_iface.so` 仅在没有设置 `HIDE_IFACE` 环境变量时退化为空壳——不拦截任何东西、直接透传原始 `getifaddrs()` 结果。对不需要隐藏网卡的进程无任何影响。

### 步骤 4：配置 rjsupplicant

在 `/opt/rjsupplicant/x64/run.sh` 中添加：

```bash
#!/bin/bash
export LD_LIBRARY_PATH=/opt/rjsupplicant/x64/lib
export HIDE_IFACE=tailscale0,lan0       # ← 加这一行（换成你的 LAN 口网卡名）
cd /opt/rjsupplicant/x64
exec ./rjsupplicant -u 你的学号 -p '你的密码' -n eth0 -a 1 -d 1
```

| 隐藏项 | 原因 |
|--------|------|
| `tailscale0` | `link/none` 虚拟网卡，无 MAC → SIGSEGV |
| `lan0` | 软路由 LAN 口（按实际接口名替换），双网卡被服务器封禁（见下注） |

> **补充说明——为什么要隐藏 LAN 口：** 如果你的业务场景有多个网卡同时 UP（比如软路由场景：LAN 口 + WAN 口），锐捷 802.1X 后台检测到多网卡可能判定为**共享网络**并封禁账号。这与本文 SIGSEGV 无关，但 `HIDE_IFACE` 顺手解决。

如果没有多网卡问题，只需：

```bash
export HIDE_IFACE=tailscale0
```

### 步骤 5：重启

```bash
sudo systemctl restart rjsupplicant
sudo systemctl status rjsupplicant   # 应该显示 active
```

### 验证

```bash
# 确认没再崩溃
sudo dmesg | grep segfault

# 确认认证成功
ping -c 3 8.8.8.8
```

### 修复效果

```
修复前:                              修复后:
┌────────────────────┐              ┌──────────────┐
│ lo                 │              │ lo           │
│ lan0 (LAN口)       │              │ eth0 (WAN口) │ ← 仅剩认证网卡
│ eth0 (WAN口)       │              └──────────────┘
│ tailscale0         │ NULL MAC      rjsupplicant 看到的世界
│   link/none        │ → SIGSEGV     只有安全接口，不崩溃
└────────────────────┘
```

---

## 路线 B：直接停止虚拟网卡

临时方案，不需要编译代码。

```bash
# 暂停 Tailscale
sudo tailscale down          # tailscale0 消失
# 或停止 WireGuard
sudo wg-quick down wg0       # wg0 消失

# 重启锐捷认证
sudo systemctl restart rjsupplicant
```

**缺点：** 每次重启都要操作，Tailscale/WireGuard 用不了。适合快速验证问题是否由虚拟网卡引发，验证完建议走路线 A。
