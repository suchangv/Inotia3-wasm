<p align="right">
  <a href="https://suchangv.github.io/inotia3-wasm/"><b>▶ 在浏览器里试玩 · Play in the browser</b></a>
</p>

# 艾诺迪亚 3 → WebAssembly

[English](README.md) · [中文](README.zh-CN.md)

在浏览器和掌机上运行 **艾诺迪亚 3**（Com2uS）的 Android 原版：直接执行它的 ARM
`libgame.so` —— 没有 Android，没有 Dalvik，也没有重写游戏逻辑。整个二进制由本项目的
解释器逐条执行，它向系统要的每一样东西（183 个 import、以及整套 JNI 表面）都由这个仓库提供。

**本仓库不包含任何游戏数据。** 请自备合法获得的 APK。

---

## 直接玩

<https://suchangv.github.io/inotia3-wasm/> —— 导入一次你自己的 APK（只存在浏览器的
IndexedDB 里，不会上传），然后点开始。存档可以打包成 zip 导出/导入。可以装成 PWA，
首次加载后离线可用。

## 自己编译

```sh
tools/build.sh native      # 桌面无窗口跑（测量与 oracle 基准）
tools/build.sh check       # 同上，guest 内存带越界检查
tools/build.sh wasm        # freestanding wasm32：纯吞吐数字
tools/build.sh browser     # emscripten + gl4es，预加载游戏文件（开发用）
tools/build.sh web         # emscripten，不预加载任何游戏数据（线上用的就是它）
tools/build.sh sdl         # SDL2 窗口、手柄、ALSA/PipeWire 音频
tools/build.sh handheld    # aarch64 交叉编译，给 PortMaster 类掌机
tools/build_vorbis.sh      # 单独构建 Ogg Vorbis 解码模块

tools/build-android.sh     # 打 APK：arm64-v8a + x86_64，debug 签名
tools/build-android.sh --install
ABI=arm64-v8a tools/build-android.sh
```

浏览器目标需要 emscripten（默认 `$HOME/emsdk`）和为宿主编译的 gl4es
（`tools/build_gl4es.sh`）；`vendor/` 不进仓库。所以刚 clone 下来是不能直接编的 ——
自备 APK、跑完这两个脚本之后才行，细节见 [`docs/HANDOFF.md`](docs/HANDOFF.md)。

Android 目标需要 SDK 的 build-tools 与 platform、NDK、以及一个 JDK
（`ANDROID_SDK_ROOT` / `ANDROID_NDK_HOME` / `JAVA_HOME`，默认 `~/android/sdk` 和
`~/android/jdk`），外加 `ABIS="arm64-v8a x86_64" tools/build_gl4es.sh android` 产出的
静态 gl4es。**不需要 Gradle**：`android/AndroidManifest.xml`（NativeActivity，
`hasCode="false"`）+ `tools/build-android.sh` 就是全部构建系统，游戏文件由脚本拷进
`android/assets/`。细节见 [`docs/ANDROID.md`](docs/ANDROID.md)。

## 为什么是这个形态

是二进制决定的。真实 `libgame.so`（1,359,908 字节）的事实：

| 属性 | 值 | 后果 |
|---|---|---|
| `Tag_CPU_arch` | **v5TE**，不是 v6/v7 | 没有 ARMv7 专属语义要建模 |
| `Tag_THUMB_ISA_use` | **Thumb-1** | 没有 Thumb-2、没有 IT 块，16 位定长 |
| `Tag_FP_arch` | **VFPv2**，软浮点 ABI | 没有 NEON；double 走 r0/r1 寄存器对 |
| `.text` | 96% 是 Thumb | 一个解码器覆盖几乎所有代码 |
| 动态标志 | `BIND_NOW` + `SYMBOLIC`，无 `mprotect` | 不 JIT、不自修改代码 |
| 重定位 | 3,139 条，全部可在加载时解决 | 加载器完全掌握地址空间 |
| import | 183 个（58 个 GLES1，其余 libc/libm/pthread/JNI） | 宿主面小而封闭 |
| `.dynsym` | `.text` 内有 5,962 个函数 | 每个入口都可枚举 |

最后一行正是"可验证"的来源：需要证明、需要差分的函数是一个有限集合。

## 工作原理

```
libgame.so ──▶ ELF32/ARM 加载器 ──▶ 重定位 + 按地址排序的符号索引
                    │
                    ▼
              ARMv5TE/Thumb-1 解释器 ─── BKPT 桩 ──▶ 宿主 import
                    │                          └──▶ libgcc 助手函数
                    ▼
              手搭的 JavaVM/JNIEnv（放在 guest 内存里，偏移与引擎一致）
                    │
                    ▼
              GLES1 固定管线 ──▶ gl4es ──▶ GLES2 ──▶ WebGL2 / 原生 GL
                    │
                    ▼
              宿主层：emscripten · SDL2 · freestanding wasm · Android
```

* **guest 内存**是一整块 144 MB 平坦区域，guest 地址换算成宿主指针就是一次加法。
  原生构建挂了 fault handler，把 `SIGSEGV` 还原成 guest PC、地址和寄存器。
* **import** 每个占 4 字节 `BKPT` 桩，解释器从地址反推索引，所以内层循环里"统计宿主调用"
  不花钱。libgcc 的**内部**助手（`__divsi3`、`__divdf3` 等）用同一套机制挂到宿主 ——
  在 ARMv5TE 上没有除法指令，这些占了稳态指令数的 52%。
* **Java** 用一句话说不完：`JavaVM`/`JNIEnv` 是我们按引擎真实偏移在 guest 内存里搭出来的，
  native 方法回调进引擎自己的入口点。
* **GL** 是定点 GLES1（`glVertexPointerx`、`glClearColorx`…），现在的驱动都不实现。
  gl4es 把它翻译成 GLES2，本项目的 GL 代码只是 ABI 适配 + `GL_FIXED` → `float` 转换。

## 现在能跑到什么程度

| | |
|---|---|
| 启动 | 资源安装（12 个文件，LZMA）、加载器 34 个阶段、主菜单 |
| 玩法 | 菜单、UI、地图、角色、战斗 —— 这个二进制用到的 Thumb-1 / ARM / VFPv2 指令都已覆盖 |
| 文字 | 真实字体，游戏发行的所有语言（不再是引擎自带的位图字体） |
| 存档 | 浏览器里 `/data` 是 IDBFS（刷新不丢），可 zip 导出/导入，可二次确认清除 |
| 音频 | WebAudio 后端驱动 BGM 与音效；Ogg Vorbis 由浏览器解码，浏览器没有 Vorbis 解码器时（Safari）用 `vorbis.wasm` |
| 输入 | 指针/触摸、SDL 目标上的手柄、按键到引擎触摸坐标的屏幕映射 |
| 离线 | service worker 预缓存外壳与模块 |
| 设备 | 桌面浏览器、iPhone/iPad（PWA）、RK3566 掌机，以及给了本项目全部逆向结论的原生 PS Vita 移植 |

未完成项见 [`docs/REMAINING.zh-CN.md`](docs/REMAINING.zh-CN.md)，性能测量见
[`docs/PERFORMANCE.md`](docs/PERFORMANCE.md)。

## 靠验证，不靠感觉

* **差分 oracle**：`tools/difftest.sh` 把同一份已重定位的镜像跑两遍 —— 一遍用本项目解释器，
  一遍用 Unicorn 的 ARM 核并逐指令 hook —— 再用 `tools/difftrace.py` 对比轨迹。两边起点
  内存完全一致，所以不一致**必然是**解码器 bug，第一处差异直接给出指令、寄存器组和 PC。
  `--all` 会扫 `.text` 里全部 5,962 个函数入口。
* **像素 A/B**：同一个帧号、改动开/关各跑一次，逐字节比较。解释器是确定性的，这能抓到
  oracle 看不见的行为回归（"一致地算错"）。
* **guest 侧计数**：每次 import 穿越、每帧指令数与解释耗时都有测量和日志 —— armv5 除法
  助手之所以被认出来是最热的东西，就是靠这些数字。

当前数字（桌面 Chrome，游玩中）：**每个引擎帧 2,150,383 条指令、解释 12.8 ms、34 次 draw、
占墙钟 19%，跑在引擎自身的 15 fps 上** —— 约 5 倍余量。RK3566 掌机上同样是 2.27M 指令，
但只有约 27 MIPS（≈84 ms/帧），所以才有了 libgcc 宿主钩子和 `inotia3-aarch64` 目标。

## 目录

```
web/                 线上页面：index.html、sw.js、manifest、图标
android/             AndroidManifest.xml —— APK 是 NativeActivity，没有 Java
src/  guest.h        一整块 144 MB 的 guest 地址空间
      elf.h/.c       ELF32/ARM 读取、重定位、符号索引
      cpu.h/.c       ARMv5TE / Thumb-1 / VFPv2 解释器
      imports.h/.c   183 个 import：BKPT 桩、分派、穿越计数
      host_hooks.c   libgcc 助手由宿主实现（除法、软浮点）
      host_libc.c    bionic 兼容层 + guest 堆分配器
      jni.c          JavaVM/JNIEnv（建在 guest 内存里）
      gl.c, gl4es_bridge.c   GLES1 状态机 + 固定管线→gl4es 适配
      platform_*.c   宿主：native、SDL2、emscripten、freestanding wasm、Android
      main.c         各运行器
tools/               构建脚本、oracle、差分测试、页面的 JS 侧
docs/                SPIKE、PERFORMANCE、HANDOFF、REUSE、ANDROID、PORTMASTER、需求清单
third_party/stb/     stb_vorbis（音频）、stb_truetype（字体）
```

## 游戏文件不在这个仓库里

`game/`、`data/`、`*.so`、`*.apk`、`*.obb` 全部 gitignore，而且这条对**整个历史**成立：
早期几个带安装资源、帧缓冲导出的提交在仓库首次公开前被重写过，所以任何可达提交里都不存在
游戏内容的 blob。线上页面同样不含游戏数据 —— 它要玩家自己的 APK，且只落在玩家浏览器里。

## 站在别人的工作上

**[unicorn-engine](https://github.com/unicorn-engine/unicorn)** —— 正确性 oracle。
这个移植里每一处解码器修复，都是靠"和另一个独立实现逐指令对照"找到的。GPLv2，仅作测试工具。

**[gl4es](https://github.com/ptitSeb/gl4es)** —— GPU 那一半。定点入口的 GLES1 固定管线
今天没有实现，gl4es 把它翻成 GLES2，所以本项目只是适配器，不是渲染器。

**[Inotia3-vita](https://github.com/MetalSyntax/Inotia3-vita)** —— 同一引擎的原生 PS Vita
移植，也是这个项目"几周而不是几个月"的原因：guest 生命周期、JNI 偏移、移植必需的二进制
补丁、触摸坐标、`UI_listControl` 的发现，全部来自那里。

## 文档

| | |
|---|---|
| [`docs/SPIKE.md`](docs/SPIKE.md) | 量了什么、数字决定了什么 |
| [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) | 帧预算、时钟 bug、时间花在哪 |
| [`docs/HANDOFF.md`](docs/HANDOFF.md) | 详细版：每个 bug 的来龙去脉 |
| [`docs/REUSE.md`](docs/REUSE.md) | 依赖、许可证和踩过的坑 |
| [`docs/ANDROID.md`](docs/ANDROID.md) / [`docs/PORTMASTER.md`](docs/PORTMASTER.md) | 两个打包目标 |
| [`docs/REMAINING.zh-CN.md`](docs/REMAINING.zh-CN.md) | 剩余工作清单 |
