# 全志 T113 固件镜像裁剪与烧录记录（亿百特 T113S4 当读卡器 → T113S3 主机）

将 58.42 GB 的原始整盘镜像裁剪到能塞进 16 GB TF 卡的 6.1 GB 镜像，并记录过程中踩过的坑。

---

## 1. 背景与目标

- **目标主机**：另一块 **T113S3 主机**（全志 T113，ARM Cortex-A7），**无板载存储，必须从 TF 卡启动**。
- **写卡工具**：**亿百特 T113S4 开发板**（本身有板载存储、能正常跑 Linux），本次**临时把它当 TF 卡读卡器用**——加载 `g_mass_storage` 把插在它上面的 TF 卡通过 USB OTG 暴露给电脑。
- **原始刷机包**：目标主机的整盘镜像 `disk.img`，大小 **58.42 GiB**（62,723,719,168 字节 / 122,507,264 扇区）。
- **目标**：TF 卡只有 16 GB，装不下 58 GB，需要裁剪成 ≤16 GB 的可启动镜像，写好后再插到目标主机上开机。
- **约束**：手边没有独立的 TF 卡读卡器，只能借亿百特这块板子来写卡。

---

## 2. 原始镜像结构

### 2.1 分区表（Allwinner 特殊 GPT 布局）

用 `fdisk -l` 能看到 GPT，但**它不是标准 GPT**，不能随便用 `sgdisk / parted` 重建：

| 位置 | 内容 |
|---|---|
| LBA 0（偏移 0） | 全零（无保护 MBR） |
| LBA 1（偏移 512） | GPT 头 `EFI PART` |
| LBA 73726 | **分区表项真正所在处**（不是常规的 LBA 2） |
| LBA 2 | 同样存了一份分区表项（冗余副本） |
| 偏移 `0x2000`（8 KB） | **boot0（`eGON.BT0`，SPL）** |

GPT 头里的关键字段：

```
first usable LBA = 73728
partition entries LBA = 73726     ← 注意不是 2
number of entries = 8, entry size = 128
```

也就是说**前 73728 个扇区（36 MB）整块保留给引导程序**，分区表项被放在这块保留区的末尾。
👉 如果用标准工具重建 GPT（会写 LBA 2~33），就会**直接覆盖 8 KB 处的 boot0**，板子再也起不来。

### 2.2 原始分区

| # | 名称 | Start | End | 大小 | 说明 |
|---|---|---|---|---|---|
| 1 | boot-resource | 73728 | 108165 | 16.8 M | 启动资源/logo |
| 2 | env | 108166 | 110213 | 1 M | U-Boot 环境变量 |
| 3 | env-redund | 110214 | 112261 | 1 M | env 备份 |
| 4 | boot | 112262 | 147461 | 17.2 M | 内核 / U-Boot |
| 5 | rootfs | 147462 | 100810757 | **48 G** | ext4 根文件系统 |
| 6 | dsp0 | 100810758 | 100812805 | 1 M | DSP 固件（ELF） |
| 7 | private | 100812806 | 100845573 | 16 M | 全零，空 |
| 8 | UDISK | 100845574 | 122507229 | 10.3 G | 全零，未格式化 |

- rootfs 的 ext4 超级块：`Block count = 12582912`（4 KiB × 12582912 = 48 GiB），`Inode count = 3145728`，**实际只用了 489253 个块 ≈ 2.0 GB**。
- 也就是说 48 GB 的分区里 96% 是空的 —— 有巨大的裁剪空间。

---

## 3. 裁剪思路

### 3.1 ❌ 错误做法（第一版脚本，导致板子卡在 logo）

第一版脚本干的事：

1. 改 GPT：把 rootfs 分区从 48 G 改成 4 G，并把 dsp0/private/UDISK 挪到 rootfs 后面；
2. 改 ext4 主超级块的 `s_blocks_count`：`12582912 → 1048576`；
3. 按新布局截断镜像。

**为什么错：**

- **只改了数字，没有搬移数据。** ext4 的元数据（块位图、inode 表、目录项）和数据块仍按"48 GB"分布，直接改小超级块 = 宣称"文件系统只有 4 GB"，于是：
  - 目录里指向高位 inode（如 `#1925122`）的条目全部越界 → `e2fsck` 报 `invalid inode #`；
  - 分布在第 32 个块组之后的数据块直接丢失；
  - 出现 `directory corrupted`。
- **漏改了 `s_inodes_count`**（应为 `3145728 → 262144`），内核直接报：
  ```
  EXT4-fs (mmcblk1p5): inodes count not valid: 3145728 vs 262144
  ```
- 顺带把 dsp0 / private / UDISK 的数据也弄错了（与原镜像 md5 不一致）。

**结果：镜像能挂载、能列出顶层目录，但文件系统内部已损坏。亿百特插着这张卡上电时（TF 卡优先启动）直接卡在开机 logo —— 一开始误以为是板子坏了，其实是卡里的镜像坏了。**

### 3.2 ✅ 正确做法

核心思想：**必须用 `e2fsck` + `resize2fs` 真正缩小文件系统（它会自动把数据搬到前面），而不是手改超级块数字。**

完整流程：

```
# 0. 备份/定位原始镜像 backup/disk.img（58.42 GB）

# 1. 对原始 rootfs 做文件系统检查（镜像来自运行中的系统，journal 是脏的）
losetup -o $((147462*512)) --sizelimit $((100663296*512)) disk.img   # 得到 /dev/loopX
e2fsck -f -y /dev/loopX        # 修复 journal、重建 resize inode

# 2. 真正缩小文件系统到 4 GiB（这一步会把数据搬到低地址）
resize2fs /dev/loopX 4194304K

# 3. 再次检查，确认干净
e2fsck -f -y /dev/loopX

# 4. 按新布局组装镜像：
#    - 保留前 75 MB（bootloader + GPT + p1~p4）原样
#    - 把缩小后的 4 GB rootfs 放到 p5
#    - 复制 dsp0 / private 到新位置
#    - UDISK 写零（原盘就是空的）
#    - GPT 沿用原有布局（只改分区起止），绝不覆盖 boot0
```

裁剪后的布局：

| # | 名称 | Start | End | 大小 |
|---|---|---|---|---|
| 1 | boot-resource | 73728 | 108165 | 16.8 M |
| 2 | env | 108166 | 110213 | 1 M |
| 3 | env-redund | 110214 | 112261 | 1 M |
| 4 | boot | 112262 | 147461 | 17.2 M |
| 5 | **rootfs** | 147462 | 8536069 | **4 GiB** |
| 6 | dsp0 | 8536070 | 8538117 | 1 M |
| 7 | private | 8538118 | 8570885 | 16 M |
| 8 | UDISK | 8570886 | 12765189 | 2 GiB |

镜像总大小 = 12,765,190 扇区 = **6,535,777,280 字节 ≈ 6.09 GiB**。

---

## 4. 校验结果

```
# e2fsck 干净
/dev/loop0: 20583/262144 files (0.0% non-contiguous), 303169/1048576 blocks

# 挂载后容量正常
/dev/loop0  3.8G  960M  2.9G  25%  /tmp/mnt

# 与原系统文件数完全一致
small files: 20574
orig  files: 20574
diff（原始有、裁剪后没有的）: 无

# dsp0 与原镜像 md5 一致
p6 dsp0 md5: cf57ff401e364a6674c583982fc0be07（两者相同）

# boot0 魔数正常
offset 8192: f0 00 00 ea 65 47 4f 4e  ("eGON.BT0")
```

写入 TF 卡后，再在板子上直接复核一遍：

```
fdisk -l /dev/mmcblk1        # 8 个分区：rootfs 4096M / dsp0 1M / private 16M / UDISK 2048M
e2fsck -f -n /dev/mmcblk1p5  # 20583/262144 files, 303169/1048576 blocks（干净无错）
mount /dev/mmcblk1p5 /tmp/vv # 3.9G 总 / 992M 已用 / 2.9G 可用，目录结构完整
md5sum /dev/mmcblk1p6        # cf57ff401e364a6674c583982fc0be07（与原始 dsp0 一致）
```

---

## 5. 烧录方法

### 方法 A：串口 + WiFi + SSH 流式写入（本次实际采用的方案，成功）

板子有调试串口（CH340 → COM3，115200）且有 WiFi。板子系统正常运行时，可以**完全不借助读卡器**，直接把镜像"流"进 TF 卡：

```bash
# 1) 串口(115200)进入板子 shell，先把网络弄通
wpa_cli -i wlan0 status            # 确认已关联到 AP
udhcpc -i wlan0                    # DHCP 拿 IP（本次拿到 192.168.110.11）

# 2) 释放 TF 卡（若之前用过 g_mass_storage，要先卸载）
rmmod g_mass_storage
umount /dev/mmcblk1p1

# 3) 在电脑上流式写入板子的 TF 卡（板子端不需要额外空间）
dd if=disk_small.img bs=4M status=progress | \
  ssh root@192.168.110.11 'dd of=/dev/mmcblk1 bs=4M; sync'
```

实测 ≈12–13 MB/s（WiFi），6.5 GB 约 9 分钟。

> ⚠️ **重要踩坑**：Windows 上**无法**用 PowerShell/.NET 对"可移动 U 盘"做整盘裸写 —— 即使管理员权限也会报 `设备未就绪`（ERROR_NOT_READY）。所以"板子当读卡器 + 电脑裸写"这条在 Windows 下走不通，改用本方法最省事。

### 方法 B：读卡器（最通用）

```
# Windows：用 balenaEtcher / Win32DiskImager 直接写 disk_small.img
# Linux / WSL：
sudo dd if=disk_small.img of=/dev/sdX bs=4M status=progress conv=fsync
```

### 方法 C：用亿百特开发板当读卡器（USB gadget）

亿百特自己跑着 Linux（系统在**板载存储**上），加载 `g_mass_storage` 后，插在它上面的 TF 卡 `/dev/mmcblk1` 就会通过 USB OTG 暴露给电脑，电脑上多出一个 U 盘：

```bash
# 在亿百特上（通过 SSH）
umount /dev/mmcblk1p1                              # 先卸载 TF 卡
echo "" > /sys/kernel/config/usb_gadget/g1/UDC      # 断开已有的 USB gadget
modprobe g_mass_storage file=/dev/mmcblk1 removable=1 ro=0
```

然后电脑上就能像普通 U 盘一样整盘写入（Windows 下用 `write_disk.ps1`，或 Linux 下 `dd`）。

> ⚠️ 注意：亿百特上电时 **TF 卡优先启动**。一旦卡里被写入了可启动镜像，下次给亿百特上电它会优先去启动这张卡（本次就因为卡里还是坏镜像而卡在开机 logo）。所以写卡期间别重启这块板子，写完把卡拔下来即可。

### 方法 D：全志 FEL 模式（未走通，仅记录）

芯片无引导介质时会自动进入 USB FEL 模式（电脑端识别为 `VID_1F3A:PID_EFE8`），理论上可用 **`xfel`** 直接写 SD 卡：

```
xfel version                     # 确认芯片
xfel ddr t113                    # 初始化 DDR
xfel sd write 0 disk_small.img   # 从偏移 0 开始写整盘镜像
```

- `xfel` **支持 T113**（社区用它给 T113 刷 awboot），Windows 版见 releases（自带 Zadig 装 WinUSB 驱动）。
- 没走通的两个原因：
  1. Windows 上 `xfel` 需要先给 FEL 设备装 **WinUSB 驱动**（Zadig，需管理员权限）；
  2. 进入 FEL 需要"无有效引导介质"，也就是**卡不能在里面**；但要写卡又必须插卡 —— 时序上要"先拔卡进 FEL，再把卡热插回去"。

---

## 6. 踩坑记录

1. **Allwinner 的 GPT 不是标准 GPT**，分区表项在 LBA 73726，不能拿 `sgdisk/parted` 重建，否则会覆盖 8 KB 处的 boot0。改分区只能改分区表项里的起止 LBA。
2. **ext4 缩小必须走 `resize2fs`**，手改 `s_blocks_count` / `s_inodes_count` 一定会损坏文件系统。
3. 从运行中的系统 dump 出来的镜像，**journal 是脏的**，`resize2fs` 前必须先 `e2fsck -f`。
4. **亿百特上电时 TF 卡优先启动**：往它插着的 TF 卡写入可启动镜像后，下次上电它会优先去启动这张卡（本次卡里还是坏镜像，于是直接卡在 logo）。写卡期间别重启亿百特，写完及时把卡拔下。
5. 板子上被内核重新枚举的分区，**设备节点主次设备号要用 `/sys/block/mmcblk1/mmcblk1pN/dev` 里的真实值**（这里是 `179:5`），自己 `mknod` 猜号会挂载失败。
6. Windows PowerShell 脚本里**中文路径会因编码被改写**（`铝合金` 变乱码导致"路径不存在"），脚本内尽量改用纯 ASCII 路径。
7. 可移动磁盘**不能** `Set-Disk -IsOffline`（会报 `Removable media cannot be set to offline`），要先 `mountvol X: /P` 摘盘符再用 `\\.\PhysicalDriveN` 写。
8. **GPT 的备份分区表必须有地方放**：如果把镜像总大小设成"正好等于最后一个分区的末尾"，分区 8 就会压在备份 GPT 上、且 `last usable` 小于分区末尾 —— 这属于**不合法 GPT**（注意：主分区表 CRC 可以是对的，`fdisk` 也不一定报错，容易被忽略）。正确做法是镜像末尾多留 33 个扇区给备份 GPT，改完用 `sgdisk -v` 校验应显示 *No problems found*。

---

## 7. 目录结构

```
.
├── README.md                    本文档
└── scripts/
    ├── analyze.sh               分析原始镜像（容量 / rootfs 实际占用）
    ├── gpt_check.py             解析 Allwinner 特殊 GPT（头 + 两份分区表项）
    ├── dump_parts.sh            逐个查看分区类型 / 用量（dsp0 是 ELF，private/UDISK 是空）
    ├── check_boot_parts.sh      比对裁剪镜像与原镜像的引导分区是否一致
    ├── hexdump_head.sh          查看前 1 MB 的 MBR / GPT / boot0 布局
    ├── rebuild.sh               ★ 正确的裁剪重建脚本（e2fsck + resize2fs + 回填）
    ├── fix_superblock.py        （第一版错误思路的补救）修补镜像里的 ext4 超级块
    ├── fix_sb_board.py          直接在板子上修补 TF 卡里的 ext4 超级块
    ├── fix_gpt.py               ★ 修复 GPT：镜像末尾留出备份表空间 + 重算两级表/CRC
    ├── verify_gpt_crc.py        校验 GPT 头部/分区表 CRC 是否正确
    ├── verify_rebuild.sh        重建后校验（fdisk / e2fsck / 挂载 / 文件数比对）
    ├── check_all_bgs.py         检查所有备份超级块的值是否一致
    ├── check_orig_parts.sh      检查原始镜像各分区（private/UDISK 是否为空、boot 格式）
    ├── cmp_head.sh              逐块比对预留引导区 + p1~p4（只应 GPT 相关块不同）
    ├── peek_boot.sh             抠出 boot 分区头部信息
    ├── check_board.sh           查看板子系统与存储状态
    ├── check_gadget.sh          查看板子 USB gadget 状态与可用模块
    ├── enable_mass_storage.sh   把 TF 卡通过 USB OTG 暴露成 U 盘（板子当读卡器）
    ├── umount_sd.sh             卸载板子上的 TF 卡挂载点
    ├── write_disk.ps1           Windows 端：直接写 USB 磁盘（配方法 C 使用）
    └── serial_cmd.ps1           Windows 端：通过板子调试串口(COM3,115200)执行命令（方法 A 用）
```

---

## 8. 一句话总结

> 裁剪全志（Allwinner）整盘镜像时：**GPT 千万别用标准工具重建**（会覆盖 boot0），**文件系统一定要用 `e2fsck` + `resize2fs` 真缩**（别手改超级块数字）。

---

## 9. 启动链分析（从 env 分区读出来的）

`env`（p2）里存着 U-Boot 的启动配置：

```
earlycon=uart8250,mmio32,0x05000000
console=ttyS3,115200
init=/init
mmc_root=/dev/mmcblk0p5
dsp0_partition=dsp0
setargs_mmc=setenv bootargs ... root=${mmc_root} init=${init} partitions=${partitions} ...
boot_dsp0=sunxi_flash read 43000000 ${dsp0_partition}; bootr 43000000 0 0
boot_normal=sunxi_flash read 43000000 boot; bootm 43000000
bootcmd=run setargs_mmc boot_dsp0 boot_normal
```

要点：

1. **根文件系统固定是第 5 分区**（`/dev/mmcblk0p5`）→ 裁剪时 **p5 的起始扇区必须保持不变**（本次保持不变：147462）。
2. U-Boot 用 `sunxi_flash read <addr> <分区名>` **按分区名**读 `dsp0` / `boot` → **分区名和 GPT 必须完全对得上**。
3. 内核从 `boot` 分区加载，格式是 `ANDROID!`（Allwinner boot image，`bootm 43000000`）。
4. 串口参数：**ttyS3 @ 115200 8N1**（排查问题就接这个口）。

## 10. 当前状态与待办

- ✅ 镜像已修正 GPT，提供**两个版本**（都通过 `sgdisk -v`）：
  - `disk_small.img` = **6,535,794,176 字节** —— rootfs 4G / dsp0 1M / private 16M / **UDISK 2G（内容置零）**
  - `disk_full.img`  = **15,479,078,400 字节** —— 只缩 rootfs，**UDISK 保留原始 10.3GB 完整内容**（16GB 卡刚好装得下）
- 注：原盘 `UDISK` 里的数据是"每 1MB 高度重复的图案"（疑似出厂填充或加密），无法确定是否有用，故额外做一版保真镜像兜底
- ⚠️ **尚未验证能否在目标 T113S3 主机上启动** —— 主机仍然卡在开机 logo
- 待办（需要硬件）：
  1. **TF 读卡器**：把修正后的镜像写进 TF 卡（`dd` / balenaEtcher 均可）
  2. **串口线**（接主板 `ttyS3`，115200）：抓开机日志，定位卡在 U-Boot 还是内核/rootfs
- 已有线索：GPT 的备份表位置问题（已修）；`UDISK`/`private` 并非全空（UDISK 是每 1MB 重复的填充数据，非文件系统）；分区 GUID 与原始一致；预留引导区与原始逐字节一致

