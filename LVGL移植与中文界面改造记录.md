# T113-S3 拆机板：LVGL 移植 / 中文界面 / WiFi / SSH 改造记录

本文记录在 3D 打印机拆机主板（全志 T113-S3）上把原厂 Qt 界面替换为自编译 LVGL 界面的完整过程，
包括原问题定位、踩过的坑、板端改动和构建部署流程。

---

## 1. 硬件与系统现状

| 项目 | 值 |
|---|---|
| SoC | 全志 T113-S3（`sun8iw20p1`），双核 Cortex-A7，128 MB DDR |
| 启动介质 | 只有 TF 卡（无板载存储），GPT 特殊布局，boot0 在偏移 8 KB |
| 内核 | Linux 5.4.61（Allwinner BSP） |
| 系统 | Buildroot，glibc 2.25，**armhf**（`/lib/ld-linux-armhf.so.3`） |
| 调试串口 | `ttyS3` @ 115200 8N1（PC 侧为 FTDI，COM6） |
| LCD | 720×1280 @32bpp，stride 2880；fb0 虚拟高度 2560（双缓冲） |
| 触摸 | `/dev/input/event1` = `generic ft5x06`（电容，MT 协议 B） |
| WiFi | Realtek RTL8821CS（走 **SDIO**，`mmc1`），支持 5 GHz / 802.11ac |
| 屏幕节点 | `/dev/fb0`、`/dev/disp`、`/dev/g2d` |

原来的开机界面是 Qt 5.12 嵌入式程序 `/home/wqf/XC_PRO_5inch`。

---

## 2. 原问题定位：为什么"卡在开机 logo"

### 2.1 系统其实起来了

抓 UART 日志可以看到：U-Boot → 内核 → rootfs 全部正常，一路跑到 Buildroot 的
`licheepi-zero login:`，nginx / sshd / dbus / wpa_supplicant 都在运行。

**卡在 logo 的真正原因是 GUI 程序段错误崩溃**，不是引导或分区问题。

### 2.2 崩溃点

手动跑 `/home/wqf/XC_PRO_5inch`：

```
QIconvCodec::convertToUnicode: using Latin-1 for conversion, iconv_open failed
QStandardPaths: XDG_RUNTIME_DIR not set, defaulting to '/tmp/runtime-root'
Segmentation fault
```

用 `LD_DEBUG=files` + 内核 `print-fatal-signals` 定位：

| 顺序 | 现象 |
|---|---|
| 崩溃 1 | `/opt/qt5.12_embed/plugins/imageformats/libqgif.so` 初始化时跳到空指针（`PC is at 0x0`，`LR` 在库内偏移 `0x15c8`） |
| 崩溃 2 | 把 libqgif.so 移走后程序能多跑一段（开始读配置、同步时间），随后崩在 `libQt5SerialPort.so.5` |
| 旁证 | 板上 `python` / `python3` 也报 `dl-version.c: 224: _dl_check_map_versions: Assertion 'needed != NULL' failed!` |

两次崩溃地址**确定性复现**（关掉 ASLR 后 PC/LR 完全一致），排除随机硬件故障。

### 2.3 结论

**rootfs 里有多个文件的内容损坏**——`e2fsck` 是查不出来的（元数据完好），只有真正执行到那段代码才会炸。
推测与当初「58 GB 原始镜像裁剪成 6 GB 写入 16 GB TF 卡」那一环有关。

> 旁注：内核报的 `GPT:Primary header alternate_lba != Alt. header my_lba` 只是备份 GPT 的警告，
> 分区 p1~p8 都能正常枚举，**不是卡死原因**。

---

## 3. 显示 / 触摸 / 网络的关键事实

### 3.1 屏幕：面板和通路是好的，背光是硬件问题

- 直接向 `/dev/fb0` 写满数据，屏幕有反应 → 通路正常
- 填纯白后画面**中间亮、四周明显变暗**，且用户拆机时拆掉了屏幕金属外壳
- 判定：**LCD 面板与显示通路正常，背光/导光板光耦合被破坏**（硬件问题，软件改不了）
- DTB 里 `lcd_backlight = <50>`、`lcd_pwm_used = <1>`、`lcd_pwm_ch = <5>`，
  但没有 `/sys/class/backlight`，PWM 通道被 disp 驱动占用，**没有运行时调背光的接口**

### 3.2 触摸：驱动声明与实际上报不一致

ft5x06 通过 `EVIOCGABS` 声明的 `ABS_MT_POSITION_X/Y` 范围是 **0..65535**，
但实测上报的**已经是面板像素坐标**（X 4~701、Y 0~1279）。

所以**不能用 ioctl 范围去缩放**，直接把原始值当像素用。这是最容易踩的坑。

事件值（`LV_TOUCH_DEBUG=1` 观察）：

```
type=3 code=57  ABS_MT_TRACKING_ID   按下给正值，抬起给 -1
type=3 code=53  ABS_MT_POSITION_X    0..719
type=3 code=54  ABS_MT_POSITION_Y    0..1279
type=1 code=330 BTN_TOUCH            1/0
type=0 code=0   SYN_REPORT
```

### 3.3 网络

- 板子扫描到 5 GHz（信道 161，`wifi_generation=5`，`ieee80211ac=1`），已连上主机同名 SSID
- 板端缺少 `iw`，只有 `iwconfig` / `iwlist`，配网用 `wpa_cli`
- 启动日志有 `cfg80211: failed to load regulatory.db`，实测不影响 5 GHz 连接

---

## 4. 交叉编译：为什么必须静态链接

预编译的 LVGL demo 是**软浮点 armel**（`e_flags=0x05000200`，解释器 `ld-linux.so.3`），
而这块板子是 **armhf**（`ld-linux-armhf.so.3`），动态链接下 ABI 不兼容，直接跑不了。

解决办法：**用软浮点工具链编译成完全静态链接的程序**。

- 软浮点静态程序内部自洽（含自己的 libc），内核照常执行，ABI 差异不产生影响
- 顺带绕开了 rootfs 里损坏的动态库
- 代价：没有硬件浮点加速（LVGL 核心是整数运算，实测影响可接受）

同时因为板子没有 libevdev，触摸驱动是**自己写的缩减版 evdev 读取器**，不依赖任何第三方库。

---

## 5. 中文字库：为什么不能用 LVGL 内置的

LVGL 9.3 内置的 `lv_font_simsun_16_cjk` / `lv_font_source_han_sans_sc_16_cjk`
（号称"1000 常用 CJK"）**覆盖不够**，实测缺这些常用字：

```
桌 印 喷 头 热 床 进 开 暂 设 态 风 扇 关 亮 级 墨 ℃ ° 秒
```

所以改用 **`lv_font_conv` 从思源黑体（Source Han Sans SC，OFL 协议）裁剪子集**，
只打包界面真正用到的字符。

### 5.1 lv_font_conv 与 LVGL 9 不兼容（两个坑）

`lv_font_conv 1.5.3` 输出的还是 LVGL 8 时代的代码，直接编译会报错，需要两处修补
（`tools/make_cn_ui.ps1` 已自动处理）：

**坑 1：`LV_VERSION_CHECK(8, 0, 0)` 在 LVGL 9 下为假**

这个宏是**主版本严格相等**判断（`x == LVGL_VERSION_MAJOR`），LVGL 9 下恒为假，
于是字体描述符写成非 const 的 `lv_font_t`，与 `LV_FONT_DECLARE` 的 `extern const` 冲突：

```
error: conflicting types for 'lv_font_cn_32'
```

补法：

```c
#if LV_VERSION_CHECK(8, 0, 0) || LVGL_VERSION_MAJOR >= 8
```

**坑 2：`.cache` 成员在 LVGL 9 里已删除**

```
error: 'lv_font_fmt_txt_dsc_t' has no member named 'cache'
```

需要把下面整块删掉：

```c
static lv_font_fmt_txt_glyph_cache_t cache;
...
    .cache = &cache
```

### 5.2 只提取字符串字面量

生成脚本先剥掉 `//` 和 `/* */` 注释，再只从 `"..."` 里取字符。否则中文注释会被算进字库：

| 提取方式 | 字符数 | 字库体积（22px+32px） |
|---|---|---|
| 全文提取 | 164 | 291 + 540 KB |
| 只取字符串 | 78 | 172 + 315 KB |

---

## 6. 板端改动清单

### 6.1 界面程序

| 路径 | 说明 |
|---|---|
| `/home/wqf/lvgl-demo` | 静态链接的 LVGL 程序（约 1.5 MB） |
| `/etc/init.d/S99lvgl` | 开机自启脚本，支持 `start` / `stop` / `restart` |

`S99lvgl` 启动前会 `killall XC_PRO_5inch`，确保坏掉的 Qt 界面不会占着 fb0。

### 6.2 WiFi（已持久化）

`/etc/wpa_supplicant.conf` 重写为（加了 `update_config=1` 以便程序里 `save_config` 能落盘）：

```
ctrl_interface=/var/run/wpa_supplicant
ap_scan=1
update_config=1

network={
	ssid="<你的 SSID>"
	psk="<密码>"
	key_mgmt=WPA-PSK
}
```

开机由 `/etc/init.d/S40network` 加载并跑 `udhcpc`。

### 6.3 SSH（root 可直接登录）

开箱状态：`sshd` 虽在跑，但 `sshd_config` 里 `PermitRootLogin` / `PasswordAuthentication`
**都是注释状态**，走新版 OpenSSH 默认的 `prohibit-password`；而且 `/etc/shadow` 里是
`root::::::::`（**根本没有密码**），所以 root 登不上。

改动：

1. 设置 root 密码（哈希由 `mkpasswd -m sha-512 root` 生成后写入 `/etc/shadow`，
   并用 `openssl passwd -5 -salt <salt> root` 反算校验一致）
2. `/etc/ssh/sshd_config` 追加：

```
PermitRootLogin yes
PasswordAuthentication yes
```

3. `sshd` 重启

验证（在板子上自连自己，密码通过串口喂进去）：

```
root@127.0.0.1's password:
SSH_OK
uid=0(root) gid=0(root) groups=0(root),10(wheel)
```

从 PC 侧探测服务端支持的认证方式：

```
root@192.168.10.238: Permission denied (publickey,password,keyboard-interactive)
```

出现 `password` 即说明密码认证已开放。

### 6.4 应用内"连上 WiFi 就确保 SSH 开着"

`wifi_job_connect()` 成功的分支里会执行：

```c
run_cmd("mkdir -p /var/run/sshd", NULL, 0);
run_cmd("[ -f /var/lock/sshd ] || /etc/init.d/S50sshd start >/dev/null 2>&1", NULL, 0);
```

用 `S50sshd` 自己的锁文件判断，已在跑就不动，不会重复起第二个 sshd。

---

## 7. 界面功能

### 7.1 打印机主页

深蓝标题栏 + 卡片式布局，全部中文：

- 标题栏：`3D 打印机` + `WiFi` 按钮 + 状态（待机 / 打印中 / 已暂停）
- 喷头温度 / 热床温度：数值 + 进度条（定时器模拟变化）
- 打印进度：百分比 + 进度条
- 打印速度 / 剩余时间
- 风扇开关、亮度滑块
- 底部：开始 / 暂停 / 停止 三个按钮，点击切换状态

### 7.2 WiFi 页面

| 功能 | 后端实现 |
|---|---|
| 状态显示 | `当前网络` / `IP 地址` / `SSH 地址`（`root@<ip>`），每 6 秒自动刷新 |
| 扫描 | `wpa_cli scan` → `scan_results`，按信号强度排序，重复 SSID 去重 |
| 选网 + 输密码 | 全屏遮罩弹窗，固定高度键盘，密码圆点显示 |
| 连接 | `add_network` → `set_network ssid/psk` → `select_network` → 等**目标 SSID** 真的上线 → `udhcpc` → `save_config` → 确保 SSH |
| 断开 / 刷新 | 独立按钮 |

两个关键实现点：

1. **耗时操作放在独立 pthread**，主线程只轮询状态刷新界面，
   所以扫描（约 6 秒）和连接（约 12 秒）期间界面不卡
2. **连接失败自动回退**：删掉失败的配置，并恢复到连之前那个网络。
   早期版本直接用 `select_network` 会禁用旧网络，一旦失败板子就彻底离线

---

## 8. 源码结构与构建部署

```
lvgl_port/
├── main.c                 中文界面（打印机页 + WiFi 页 + 后台任务线程）
├── evdev_touch.c          触摸驱动（自写，不依赖 libevdev）
├── lv_font_cn_22.c        字库 22px（脚本生成）
├── lv_font_cn_32.c        字库 32px（脚本生成）
├── lv_conf.h              LVGL 9.3 配置
├── CMakeLists.txt         静态链接 + pthread
├── build.sh               WSL 里交叉编译，产物进 dist/
├── S99lvgl                板端开机自启脚本
├── tools/
│   └── make_cn_ui.ps1     一条命令：裁字库 → 编译 → 部署（含 sha256 校验）
└── dist/                  构建产物（lvgl-demo / .gz / .sha256）
```

### 8.1 一键构建 + 部署

```powershell
powershell -ExecutionPolicy Bypass -File tools\make_cn_ui.ps1
# 只改排版/配色（文案没变）：加 -SkipFont
# 只重新部署：             加 -SkipFont -SkipBuild
# 只构建不部署：           加 -SkipDeploy
```

脚本流程：

1. 从 `main.c` 提取字符串字面量里的非 ASCII 字符
2. `lv_font_conv` 重新裁剪字库并自动打 LVGL 9 补丁
3. 调 WSL 里的 `build.sh` 交叉编译
4. 本机起临时 HTTP 服务，板子 `wget` 拉取，串口校验 sha256 后替换并重启服务

### 8.2 依赖

- WSL2 Ubuntu 22.04 + cmake + ninja
- 易百特 `arm-buildroot-linux-gnueabi_sdk-buildroot` 工具链
- LVGL 9.3.0 源码树（`build.sh` 里默认路径 `$HOME/t113-hmi/src/lvgl-9.3.0`）
- Node.js（跑 `lv_font_conv`）
- Windows 侧 Python（起临时 HTTP 服务）

---

## 9. 常用命令

### 板端

```sh
/etc/init.d/S99lvgl restart            # 重启界面
tail -f /var/log/lvgl.log              # 看日志（/var/log 其实是 /tmp 的软链，重启会丢）

LV_TOUCH_DEBUG=1 /home/wqf/lvgl-demo   # 打印触摸原始事件
LV_ROTATION=90 /home/wqf/lvgl-demo     # 旋转界面（0/90/180/270）

wpa_cli -i wlan0 scan_results          # 看扫描结果
wpa_cli -i wlan0 status                # 看连接状态
```

### PC 侧

```powershell
ssh root@192.168.10.238                # 密码 root
scp dist\lvgl-demo.gz root@192.168.10.238:/tmp/
python -m http.server 8000 --directory dist   # 起临时分发服务
```

---

## 10. 其它发现与坑

1. **`reboot` 会挂死**：内核停在重启路径出不来（串口还在回声但没有 init），
   只能**断电重启**。重启界面请用 `/etc/init.d/S99lvgl restart`。
2. **USB 不只是供电**：
   - Host 侧可用（`usb-storage` / `usbhid` / `ch341` / `cdc_acm` / `uvcvideo` 全部内置），
     可插 U 盘 / 键盘 / USB 转串口
   - Device（gadget）侧：`configfs` 挂载后出现 `usb_gadget`，实测可用 **`mass_storage`**（把 TF 卡当 U 盘）
     和 **`rndis`**（板子当 USB 网卡）；`acm` / `ecm` / `serial` 没编进内核
   - `/lib/modules` 是空的（WiFi 驱动靠 `insmod /root/8821cs.ko`），但上述功能都编在内核里
3. **`/var/log` 是 `/tmp` 的软链接**，所以 `/var/log/lvgl.log` 就是 `/tmp/lvgl.log`，重启即丢。
4. **PowerShell 5.1 对无 BOM 的 `.ps1` 按 ANSI 解析**，中文注释会破坏语法，
   `.ps1` 必须存成 **UTF-8 with BOM**。
5. **`Start-Process -ArgumentList` 不会自动加引号**，本工程路径含空格
   （`3D打印机 T113S3拆机版`），传 `--directory` 会被空格切开；
   改用 `-WorkingDirectory` 规避。
6. 板子上的 `python` / `python3` 因 rootfs 损坏无法运行，脚本类工作都放到 PC/WSL 侧。

---

## 11. 已知问题 / 待办

- [ ] **背光偏暗且不均匀**（中间亮四周暗）——硬件问题，换屏后可考虑调 DTB 的 `lcd_backlight`
- [ ] **rootfs 文件损坏无法修复**：原厂 Qt 界面、`python` 等仍不可用。
      彻底解决需要用一份完好的原厂镜像重刷 TF 卡
- [ ] `lcd_fb0` / disp 图层未做深度适配，交互复杂度上来后可能需要接 **G2D 硬件加速**
      （当前是纯软件渲染）
- [ ] 屏幕背光/亮度滑块目前只是 UI，没有真正接到硬件
- [ ] 打印相关的温度、进度、速度都是**模拟数据**，还没接实际的打印机主控
