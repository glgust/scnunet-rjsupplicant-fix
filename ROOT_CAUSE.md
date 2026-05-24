# 根因分析

## 为什么虚拟网卡会让锐捷崩溃？

rjsupplicant 启动后做的第一件事是**遍历系统所有网卡**——找到认证网卡对应的 MAC 地址，以及检测是否有多网卡违规。

问题出在 `get_nics_info()` 这个函数里。它通过 `getifaddrs()` 获取网卡链表，然后遍历每个接口取 MAC 地址。代码写于 2014 年，做出了一个在当时成立、在今天不成立的假设：

> **"只要不是 loopback，这个接口就有 MAC 地址。"**

2014 年的 Linux (`2.6.x`) 没有 `link/none` 类型的虚拟网卡。Tailscale 的 `tailscale0`、WireGuard 的 `wg0` 都是 TUN 接口，不需要链路层地址——它们工作在 IP 层。对于这类接口，`getifaddrs()` 返回的结构中，`ifa_addr`（链路层地址指针）为 **NULL**。

rjsupplicant 的代码：先检查 `ifa_flags` 是否标记 `IFF_LOOPBACK`（环回标志），不是就当作有 MAC → 直接解引用 `ifa_addr` → NULL pointer dereference → **SIGSEGV**。

---

## 反汇编证据

用 `objdump` 对 rjsupplicant 二进制做反汇编，定位崩溃地址 `0x451999`，对应函数 `get_nics_info+0xc9`：

```asm
; 函数 get_nics_info() 循环体
; 基址偏移 +0x80 ~ +0xe4

  451950:  ...                          ; 循环入口
  451990:  41 f6 45 10 08              ; testb  $0x8, 0x10(%r13)
  451995:  49 8b 45 18                 ; mov    0x18(%r13), %rax
  451999:  44 0f b7 30                 ; movzwl (%rax), %r14d   ← CRASH
```

逐条解释（x86-64 AT&T 语法）：

```
testb $0x8, 0x10(%r13)
    %r13  → 当前遍历到的 struct ifaddrs 节点
    +0x10 → ifa_flags 字段
    $0x8  → IFF_LOOPBACK = 0x8
    含义: 检查 "这是 loopback 接口吗？"

mov 0x18(%r13), %rax
    +0x18 → ifa_addr 字段（struct sockaddr *）
    含义: 取接口的链路层地址指针

movzwl (%rax), %r14d
    (%rax) → 解引用 ifa_addr 指向的数据
    movzwl → 读 16-bit（取 sa_family，即地址族类型）
    ┊
    ┊ 如果 ifa_addr = NULL → (%rax) = *(0x0)
    ┊ → 访问 0x0 地址 → SIGSEGV
    ★ 崩溃就发生在这里
```

### 确认地址

```bash
$ sudo dmesg | grep segfault
rjsupplicant[...]: segfault at 0 ip 0000000000451999 ...
                                 ↑              ↑
                          尝试读 0x0    崩溃地址 = 451999
```

`segfault at 0` 与反汇编中 `(%rax)` = `*(0x0)` 完全一致，闭环。

---

## `struct ifaddrs` 布局（x86-64 ABI）

`getifaddrs()` 返回一个 `struct ifaddrs` 链表，每个节点描述一个网卡。在 64 位 Linux 上：

```
偏移    大小    字段         说明
──────────────────────────────────────────
+0x00   8B     ifa_next      下一个节点指针（NULL = 链表尾）
+0x08   8B     ifa_name      ⭐ 接口名称 (const char *)
+0x10   4B     ifa_flags     ⭐ 接口标志 (unsigned int)
+0x14   4B     (padding)     对齐填充
+0x18   8B     ifa_addr      ⭐ 链路层地址 (struct sockaddr *)
+0x20   8B     ifa_netmask   子网掩码
+0x28   8B     ifa_ifu       广播/点对点地址（union）
+0x30   8B     ifa_data      私有数据
```

崩溃相关的三个字段：

| 字段 | 偏移 | tailscale0 的值 | rjsupplicant 的操作 |
|------|------|:--:|------|
| `ifa_flags` | +0x10 | `0x1091` (IFF_LOOPBACK 未置位) | `testb $0x8, ...` → 通过（不是 loopback） |
| `ifa_addr` | +0x18 | **NULL** | `mov ..., %rax` → RAX = 0 |
| `*ifa_addr` | — | 不存在 | `movzwl (%rax), ...` → 读 0x0 → SIGSEGV |

---

## 崩溃链路（从头到尾）

```
rjsupplicant 启动
  │
  └→ main()
       └→ 认证准备
            └→ get_nics_info()          ← 遍历网卡
                 └→ getifaddrs()        ← 系统调用，返回网卡链表
                      │
                      返回的链表:
                      ┌──────────────┐
                      │ lo           │ ifa_flags 含 IFF_LOOPBACK → 跳过（安全）
                      │ eth1 (LAN口) │ 有 MAC，正常读取（安全）
                      │ eth0 (WAN口) │ 有 MAC，正常读取（安全）
                      │ tailscale0   │ link/none，ifa_addr = NULL
                      └──────────────┘
                            │
                            ▼
                      get_nics_info() 遍历到 tailscale0
                            │
                      testb $0x8, +0x10
                            │
                      IFF_LOOPBACK = 0（不是 loopback）
                            │
                      ┌── 不跳过，继续处理 ──┐
                      │                      │
                      ▼                      ▼
                  mov +0x18, %rax      代码以为：
                  RAX = NULL           "非 loopback = 有 MAC"
                      │
                      ▼
                  movzwl (%rax), %r14d
                  *(0x0000000000000000)
                      │
                      ▼
                  ╔══════════════╗
                  ║  SIGSEGV    ║
                  ║  at address ║
                  ║  0x0 (NULL) ║
                  ╚══════════════╝
                      │
                      ▼
                  systemd: Restart=always
                      │
                      ▼
                  get_nics_info() 又遇到 tailscale0...
                      │
                      ▼
                  无限崩溃循环
```

---

## 接口属性验证

```bash
$ ip -d link show tailscale0
4: tailscale0: <POINTOPOINT,MULTICAST,NOARP,UP,LOWER_UP> mtu 1280 ...
    link/none                              ← 无链路层地址
    tun type tun pi off vnet_hdr on ...    ← TUN 虚拟设备
```

关键字段：

| 属性 | 值 | 含义 |
|------|-----|------|
| `link/none` | 无链路层地址 | `ifa_addr` = NULL，触发崩溃的直接原因 |
| `tun` | 隧道接口 | 工作在三层（IP），不需要 MAC |
| `POINTOPOINT` | 点对点模式 | 与 `IFF_LOOPBACK` (0x8) 不同，不会被跳过 |

---

## 为什么 2014 年的代码会出这个问题？

```
2014 年 Linux 内核 (~2.6.9 / ~3.x)
  ┌─────────────────────────────┐
  │ 网卡类型:                    │
  │  - 物理网卡 (eth0, eth1...) │ 全有 MAC
  │  - 环回 (lo)                │ 被代码跳过
  │                             │
  │ link/none 接口: 不存在       │ ← 所以代码假设成立
  └─────────────────────────────┘

2024 年 Linux 内核 (~6.8)
  ┌─────────────────────────────┐
  │ 网卡类型:                    │
  │  - 物理网卡                  │ 全有 MAC
  │  - 环回                      │ 被代码跳过
  │  - TUN 虚拟网卡              │ ← link/none，无 MAC
  │   (tailscale0, wg0, tunX)  │ ← 代码不认识这种类型
  └─────────────────────────────┘
```

这是一个**前向兼容性 bug**：代码遵循了当时内核的设计约定，但内核在十年间引入了新接口类型，旧假设在新环境下不再成立。修复（`HIDE_IFACE`）通过拦截 `getifaddrs()` 屏蔽问题接口，而不是修补 rjsupplicant 本身——因为 rjsupplicant 是闭源二进制，无法直接修改。

---

[← 回到 README](README.md) | [下一章：修复方案 →](FIX.md)
