# 华南师范大学校园网：锐捷客户端 rjsupplicant 在 Tailscale/WireGuard 虚拟网卡环境下的 SIGSEGV 崩溃分析与修复

## 引言

### 环境

华南师范大学校园网采用锐捷 802.1X PEAP 认证，需要在 Linux 主机上运行 `rjsupplicant` 客户端完成入网认证。许多同学在配置软路由/NAS/开发机时会同时部署 Tailscale（远程访问）或 WireGuard（异地组网），但这两种软件创造的虚拟网卡会让锐捷客户端**直接崩溃**。

笔者的设备环境如下：

| 组件 | 版本/型号 |
|------|----------|
| 主机 | x86_64 软路由 |
| 系统 | Ubuntu 24.04.4 LTS |
| 内核 | 6.8.0-100-generic（x86-64） |
| 锐捷客户端 | `rjsupplicant`（2014 编译，ELF 64-bit，for GNU/Linux 2.6.9） |
| Tailscale | v1.98.3 |
| 校园网认证 | 802.1X PEAP，WAN 口网卡 |

> **关于接口命名：** 本文使用通用名称指代网卡，请根据你的实际环境替换：
> - `eth0` → 认证网卡（WAN 口，本例实际为 `enp2s0`）
> - `lan0` → 内网网卡（LAN 口，本例实际为 `enp1s0`）

## 现象

某次系统更新后安装 Tailscale，重启后锐捷认证**无限崩溃**：

```bash
$ sudo systemctl start rjsupplicant
$ systemctl status rjsupplicant
● rjsupplicant.service - Ruijie 802.1X Supplicant
   Active: activating (auto-restart)  ← 反复重启
```

`dmesg` 中刷出重复的段错误：

```
[40852.002827] rjsupplicant[132540]: segfault at 0 ip 0000000000451999 \
  sp 00007292fab348f0 error 4 in rjsupplicant[400000+10a000] likely on CPU 1
```

| 字段 | 值 | 含义 |
|------|-----|------|
| `segfault at 0` | NULL | 尝试读取内存地址 0x0 |
| `ip 0000000000451999` | 崩溃指令地址 | `get_nics_info+0xc9` |
| `error 4` | 位掩码 0b100 | 用户态读不存在页 |
| `rjsupplicant[400000+10a000]` | 基址+代码段大小 | ELF 文件偏移 = 0x451999 |

> **TL;DR**：只要装了 Tailscale 或任何产生 `link/none` 类型虚拟网卡（TUN）的软件，rjsupplicant 就会在遍历网卡时碰上**没有 MAC 地址的接口** → 代码未做空指针检查 → `SIGSEGV` 崩溃 → systemd 无限重启。

## 先验证：是不是同一个 bug？

在继续往下看之前，花 30 秒确认你的问题和本文是同一个。

### 一键诊断

```bash
# 1. 检查崩溃特征
sudo dmesg | grep "segfault at 0 ip 0000000000451999"
```

**如果有输出** → 100% 是本文的 bug，`get_nics_info+0xc9` 空指针解引用。**跳去 [修复](FIX.md)。**

```bash
# 2. 如果没有上面那条，检查是否有 link/none 接口
ip -d link show | grep "link/none"
```

**如果有输出** → 你撞的同类型 bug，虚拟网卡没有 MAC 地址。不同版本的 rjsupplicant 崩溃地址可能不同，但根因一致。

```bash
# 3. 顺手查：rjsupplicant 当前能看到几个网卡
sudo systemctl stop rjsupplicant
sudo -E rjsupplicant -u 你的学号 -p 你的密码 -n eth0 -a 1 -d 1 -N 2>&1 | head -5
# (-N 是 debug 模式，会打印遍历到的网卡列表)
```

**如果网卡列表里有 `tailscale0` 或 `wg0`** → 这些就是元凶。记下接口名，等会修复要用。

---

确认完再往下看根因。不确认也没关系，分析过程本身就是证据。

[下一章：根因分析 →](ROOT_CAUSE.md)
