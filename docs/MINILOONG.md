# 迷你龙（RK3566）PortMaster 移植方案

目标：把 Inotia 3 做成一个 PortMaster 游戏，放进
`D:\miniloong\roms\ports\`，启动方式与 `K_空洞骑士[中].sh` 同类。

本文是**方案**，不含实现。先说结论，再说为什么，最后是分阶段计划。

---

## 0. 结论

可行，但**"CPU 解释层可以不要"这个前提在当前形态下不成立**，这是整件事里
最重要的一句话，先解释清楚再谈别的。

### 为什么解释器不能去掉

`libgame.so` 是 **ELF32 / ARM（e_machine=40）**，那是 Android 的 **armeabi**
（ARMv5TE 软浮点）32 位 ABI。

而 miniloong 的系统（CFW=Loong）是 **aarch64** 用户空间：

- 空洞骑士的 `port.json` 里 `"arch": ["aarch64"]`；
- 所有现成 port 的二进制都是 `*.aarch64`（`gmloadernext.aarch64`、
  `mkxp-z.aarch64`）；
- SD 卡上**没有** `ld-linux-armhf.so.3` 之类的 32 位动态加载器。

所以"原生支持 ARM"这句话是对的，但指的是 **aarch64**。一个 32 位 ARM 的
`.so` 在这个系统上**没法直接跑**，除非系统带 32 位兼容层（没看到）。

三条路，我的建议是第一条：

| 方案 | 做法 | 评价 |
|---|---|---|
| **A. 保留解释器，编成 aarch64 原生程序**（推荐） | 现在的 C 解释器交叉编译成 `inotia3.aarch64`，宿主跑在 64 位，只有 **guest 代码**是 32 位 ARM | 零新风险。解释器本身与宿主位宽无关，现有代码一行不用改 |
| B. 让 guest 也原生跑 | 需要一个 32 位 ARM 用户空间（rootfs + qemu-user 或容器） | 引入一整套 32 位环境，体积大、易崩，收益只是省掉解释开销 |
| C. 静态 AOT 翻译成 aarch64 | 把 Thumb 翻译成 aarch64 机器码 | 工程量最大，且现在**完全不需要** —— 见下面的性能账 |

**性能账**：浏览器里解释器跑 15 fps 的游戏只占 19% 墙钟；native x86 上
是 289 MIPS。RK3566 是 4×Cortex-A55 @1.8GHz，比任何浏览器环境都快，
而引擎本身**设计上就是 15 fps**（`APPINFO_nTimer` = 66 ms）。
所以 A 方案在性能上有大量余量，不需要 AOT。

> 换句话说：解释器不是"没必要的包袱"，它是让这个 32 位 guest 能在 aarch64
> 上跑起来的**唯一**手段。保留它反而让移植变小了 —— CPU 层完全不用碰。

---

## 1. 需要新写的部分

现有代码已有三个完整宿主：`platform_native.c`（无窗口，测量用）、
`platform_emscripten.c`（浏览器）、`platform_android.c`（EGL + NativeActivity）。
迷你龙需要第四个，和 Android 那个最像。

| 层 | 现状 | 迷你龙要做什么 |
|---|---|---|
| CPU / ELF / JNI / 引擎 | 完成 | **不动** |
| GL | gl4es（`NOEGL` + `DEFAULT_ES=2`），已能在 NDK 下构建 | 加一个 aarch64 交叉编译目标 |
| 窗口 | Android: EGL + `ANativeWindow` | 新写 `platform_loong.c`：KMSDRM 或 wayland |
| 输入 | Android: `AInputQueue` | 新写：SDL2 手柄 / evdev |
| 音频 | Android: OpenSL ES | 新写：ALSA 或 SDL2 audio（复用 `sound_android.c` 的 stb_vorbis 解码） |
| 文字 | `text_android.c`：stb_truetype + `/system/fonts` | 复用，但字体路径要改（Loong 没有 Android 字体目录） |
| 启动器 | 无 | PortMaster `.sh` + `port.json` + `.gptk` |

### 1.1 窗口：优先 KMSDRM，不碰 wayland

空洞骑士的日志里有一句非常重要的话（在它内联的 `audio_setup` 注释里）：

> MiniLoong (CFW=Loong, wayland/weston): 系统已经跑着 音视频 和 XDG_RUNTIME_DIR
> 指向 wayland socket (/run)。**别碰任何一个** —— 覆盖 XDG_RUNTIME_DIR
> 会让加载器连不上 wayland → 黑屏 / 90° 旋转。

这说明 Loong 是个 **weston 合成器**在跑。对我们是好消息也是坏消息：

- 好消息：屏幕已经被合成器接管，我们只要拿一个 EGL surface；
- 坏消息：直接抢 KMS 会和 weston 打架。

建议**先试 KMSDRM**（SDL2 `SDL_VIDEODRIVER=kmsdrm`），因为它最省事、
PortMaster 生态里最成熟；如果 weston 占着 DRM master 导致 `EACCES`，
退回 wayland。两个都由 SDL2 处理，我们只写一次代码：

```
SDL_Init(SDL_INIT_VIDEO)
SDL_CreateWindow(960x544 → 等比缩放/letterbox 到面板)
SDL_GL_CreateContext()   ← gl4es 在这里拿 driver 的 EGL/GLES 入口
```

**屏幕尺寸**：面板是 **960×720**（空洞骑士日志 `DISPLAY_WIDTH=960
DISPLAY_HEIGHT=720`；dmesg 里 `720x960p60` 是竖着报的）。但空洞骑士的
**游戏内渲染是 960×544**，说明这类 16:10 面板和我们的 960×544 恰好
**等宽**，只是上下要留 176 px 的黑边。这是个很好的巧合：横向不需要缩放，
只有纵向 letterbox。Android 版已经实现了 letterbox 逻辑（`platform_android.c`
把引擎的 `glViewport` 映射进最大的居中 960:544 矩形），可以直接搬。

### 1.2 输入：这是第一步要交付的东西

按你的要求，分两部分。

**(a) 左摇杆 = 鼠标指针**

- 摇杆推动 → 指针按速度移动（不是绝对定位；游戏 UI 是 480×320 的点击区域，
  绝对映射没法点到精确位置）
- 指针用一个小光标绘制（引擎不知道鼠标的存在，所以要在我们自己的 GL 层画，
  或者在 gl4es 之后叠一个 sprite）
- **3 秒无操作自动隐藏**：计时器在宿主侧，不动就置 `visible=0`。任何摇杆
  移动 / 按键都重置它
- **摇杆下压（L3）= 鼠标左键点击**：按下时发 `INPUT_DOWN`，抬起发 `INPUT_UP`。
  这里有个必须注意的点——现有 `input.c` 的槽位状态机要求 DOWN/UP 成对，
  而且一次 tick 可能长达几十毫秒，所以点击要**打进同一个 FIFO**，
  不能直接调 guest

**(b) 剩下的键 = Vita 键位的等价物**

Vita 版的 `source/wr_touch.c` 里 `BUTTON_MAP` 是可用的参考，它把每个物理键
映射到屏幕上一个坐标（也就是"点那个按钮"）。迷你龙是 ABXY 布局，对应关系：

| Vita | 用途 | 迷你龙 | 目标屏幕坐标（960×544） |
|---|---|---|---|
| CROSS | 主攻击 | **A** | (880, 470) |
| CIRCLE | 技能 | **B** | (917, 365) |
| SQUARE | 技能 | **X** | (757, 427) |
| TRIANGLE | 道具/盾 | **Y** | (827, 365) |
| LTRIGGER | 道具槽 1 | **L1** | (540, 499) |
| RTRIGGER | 道具槽 2 | **R1** | (640, 499) |
| SELECT | 道具槽 3 | **L2 或 Select** | (735, 499) |
| START | 暂停 | **Start** | (42, 272) |
| D-pad | 移动 | **十字键** | (140,341)/(145,507)/(42,417)/(245,417) |

注意 Vita 的四个面键在游戏里是**四个不同的屏幕按钮**，不是 ABXY 的
"确认/取消"语义。所以 ABXY 的物理位置要和 Vita 的图形键位一致
（A 在右下 = CROSS 等），否则手感会错位。

**D-pad 移动**：Vita 的 D-pad 同样是"点屏幕上的方向键"，持续按住就是持续按住。
这一点 `input.c` 的槽位模型已经支持（按住某槽不释放）。

### 1.3 音频

Android 版已经把 `stb_vorbis` 解码器接好了（`sound_android.c`），而游戏只
播放 Ogg Vorbis。解码部分**直接复用**，只把输出从 OpenSL ES 换成 ALSA
（`snd_pcm_writei`）或 SDL2 audio。建议先用 SDL2 audio，因为 SDL 已经在
窗口层引入了，少一个依赖。

---

## 2. 工具链

需要一份 aarch64 的 glibc 工具链。三个选择：

1. **Bootlin 预编译工具链** —— 最省事，`aarch64-buildroot-linux-gnu`，
   下载解压即用，无网络依赖。**建议这个。**
2. **系统 `aarch64-linux-gnu-gcc`** —— WSL 里 `apt install gcc-aarch64-linux-gnu`，
   最快，但 glibc 版本可能比设备新（空洞骑士 `port.json` 里有
   `"min_glibc"` 字段，说明这里有个底线要求）。
3. **设备自带的 SDK** —— SD 卡上没找到，跳过。

构建产物需要**静态链接** C 运行时（或至少只依赖设备上确认存在的 glibc 版本），
否则会遇到 `GLIBC_2.xx not found`。**静态链接 glibc 有坑**（`getaddrinfo`、
`dlopen` 等），更稳的是动态链接但用较老的 sysroot。

gl4es 也要用同一个工具链重编一版（现有的 `build_gl4es.sh` 加一个 `loong`
目标即可，它已经抽象好了 `DEFAULT_ES=2` / `NOEGL` 那套参数）。

---

## 3. PortMaster 交付形态

按空洞骑士的结构：

```
roms/ports/
├── K_艾诺迪亚3[中].sh          ← 启动脚本（顶层，Ports 菜单里显示的就是它）
└── inotia3/                    ← 我们的目录
    ├── inotia3.aarch64         ← 主程序
    ├── libgame.so              ← 用户自备（版权）
    ├── assets.apk              ← 用户自备（版权）
    ├── port.json               ← 元数据
    ├── inotia3.gptk            ← 手柄 → 键盘/输入映射
    ├── [中]艾诺迪亚3.png        ← 封面
    └── data/                   ← 运行时写入（存档等）
```

**关键约定**（从空洞骑士抄来的，都是踩过坑的）：

- `controlfolder` 发现顺序是四选一，必须照抄那段 preamble
- `get_controls` 之后 `DISPLAY_WIDTH` / `DISPLAY_HEIGHT` 才有值
- **日志用 `exec > file 2>&1` 直接重定向，不用 `tee`** —— 注释里写了
  tee 在 SIGKILL 时会丢掉最后几 KB，而那正是最需要数据的时候
- **CFW=Loong 时不要碰 XDG_RUNTIME_DIR**（会切段 wayland）
- 退出时 `trap` 抓 `dmesg` 尾部（OOM 证据）
- `pm_finish` 收尾，`pm_platform_helper` 在启动长时间运行的程序前调用

`.gptk` 的写法：我们这个游戏**不需要** gptokeyb 做键盘映射（输入直接由
SDL2 手柄读，坐标是内部算的），但如果要用 PortMaster 的统一输入路径，
给一个空的 `.gptk` 或者只映射热键（退出组合键）即可。

---

## 4. 分阶段计划

按"第一步先让它跑起来"的要求：

### 阶段 1：能启动、能出画面（这一步的目标）

1. 用 Bootlin 工具链编一个 **`inotia3.aarch64`**：复用现有全部核心，
   新写 `platform_loong.c`（SDL2 窗口 + gl4es 上下文 + 一个最小输入）
2. 在本机交叉编译验证能链接、不崩（用 qemu-aarch64 跑不了 GL，
   但能验证它走到 "需要 GL" 那一步之前）
3. 拷到 SD 卡，写 `.sh` + `port.json`
4. **验收标准**：能看到 Google Play 启动画面 / com2uS 标志 / 主菜单

到这一步输入只需要**能退出**（映射一个键到 SIGTERM），摇杆和按键是阶段 2。

### 阶段 2：输入

- 左摇杆 + 光标 + 3 秒隐藏 + L3 点击
- ABXY / D-pad / Start 的坐标映射（上面那张表）
- 验收：能点进 NEW GAME，走到第一张地图

### 阶段 3：音频 + 收尾

- ALSA / SDL2 audio 后端（复用 stb_vorbis）
- 字体路径（`text_android.c` 的 `/system/fonts` 在 Loong 上不存在；
  用 PortMaster 常见的 `$GAMEDIR/*.ttf` 或系统字体路径兜底）
- 存档目录、退出清理

---

## 5. 目前还不确定、需要上机确认的事

这几条我没法从桌面环境验证，是阶段 1 的**首要排查清单**：

1. **weston 是否占着 DRM master** —— 若是，KMSDRM 起不来，得走 wayland
2. **设备的 glibc 版本** —— 决定工具链选型，否则 `GLIBC_2.xx not found`
3. **libmali / GLES 驱动名字** —— RK3566 通常走 Mali-G52，Loong 上
   可能是 `libmali.so` 或标准的 `libEGL.so`；gl4es 需要一个能用的 GLES2 后端
4. **面板真实分辨率与旋转** —— dmesg 报 `720x960p60`（竖），但 UI 是横的
   960×720。若系统做了 rotate-90，我们拿到的 surface 可能是竖的
5. **是否有 32 位兼容层** —— 如果有（`/lib/ld-linux-armhf.so.3`），B 方案
   会突然变得可行；但按现有证据判断应该没有

---

## 6. 一句话总结

**别去掉解释器** —— 它是 32 位 guest 能跑在 aarch64 上的唯一手段，而性能上
完全够用（游戏本身就是 15 fps）。真正要写的是第四套宿主：SDL2 窗口 +
gl4es + 手柄输入，外加 PortMaster 的打包外壳。阶段 1 的交付物是
"能在迷你龙上看到主菜单"。
