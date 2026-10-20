# 剩余工作清单（2026-10-21 实测）

本文是"浏览器里已经能顺利跑起来之后，还剩什么"的一份盘点。所有条目都在本机重新跑过验证，
不是从旧文档抄的。旧的 `docs/HANDOFF.md`「Where things stand」和 `docs/PERFORMANCE.md`
「What is left」两节已经过期（还写着音频未接、文字不渲染、浏览器 trap 在
`gl4es_glLoadIdentity`），以本文为准。

一句话结论：**目前跑通的是主干** —— 启动 → 菜单 → 新游戏 → 开场事件 → 第一张地图。
主干之外的存档闭环、浏览器持久化、若干宿主调用、Android 验收和工程收尾都还没做完。

---

## 0. 本次复测的基线（先看这个，判断"还没坏"）

| 项目 | 命令 | 结果 |
|---|---|---|
| native 全链路到 play | `./build/spike game/libgame.so --boot --frames=6500 --no-profile` + handoff 里的 6 个 `--tap` | `final pc=0badf00c`；19465 次 draw、51 个纹理、1271 次引擎帧 |
| 差分测试 | `tools/difftest.sh --all` | 5904 个入口 / 3,951,988 条指令，**全部一致**（capped 25、bad-mem 18 属预期） |
| 浏览器实时运行 | `node tools/watch_run.mjs http://127.0.0.1:8080/spike.html 100` | 101 秒到 frame 5803（≈57 tick/s），17 秒内出主菜单，画面完整全彩 |

浏览器这一条是**实时模式**（不是 `--virtual-time-budget`），所以它证明的是真的能连续跑，
而不是虚拟时间下的假象。

### 运行期真正"被调用但没实现"的东西（只有这些）

- import 6 个：`lrand48`(54 次/轮)、`raise`(46)、`statfs`(1)、`fsync`(21)，
  外加两个已经被 patch 顶掉的 `host/File::GetFullPathName`、`host/ParseOffsetAndLengthAndPath`。
- JNI 2 个：`NewObjectArray`(1)、`SetObjectArrayElement`(10)。

其余 147 个 import 和 400+ 个 JNI 函数都有实现。

---

## P0 — 存档 / 读档没有闭环

- 进入 play 之后 `save0.dat` 仍然不存在；`data/` 下的两个 Hub 存档目录是空的。
  `SAVE_SaveData` / `SAVE_SaveDataAsKey` 都在 `.dynsym` 里，但**从来没有真正存过一次、读回一次**。
- 更关键的是底层：一批文件系统 import 根本没实现 ——
  `opendir` / `readdir` / `closedir`、`remove` / `unlink` / `rename` / `lstat`、
  `statfs`（返回 0 但结构体不填）、`fsync`（空操作）。
  存档列表枚举、删档、备份都走这些路径，**会静默失败，不会报错**。
  这类"看起来成功"的桩比直接崩更危险。
- 工作区里 `src/main.c` 有一处**未提交的临时探针**（`g_frames_done == 6900/7000` 时
  dump 控制列表，注释写着 "Removed before committing"），说明另一个 session 正在定位存档菜单。
  接手前先看 `git status` / `git diff`，别覆盖它。

## P0 — 浏览器端没有持久化

`host_app_data_dir()` 用的是 emscripten 的 **MEMFS**，全仓库没有 IDBFS / OPFS / `FS.syncfs`。
两个后果：

1. **刷新页面 = 存档全丢**，浏览器版的"存档"目前没有意义；
2. 每次刷新都要重跑 12 个数据文件的安装 —— 这就是开机 17 秒的来源。

修法：把 IDBFS 挂到 `/data`，在 `pagehide` / 定时器里 `syncfs`。做完之后第二次进游戏，
开机时间应该从十几秒掉到几秒，存档也才有地方落。

## P1 — 宿主面剩下的洞 —— **2026-10-21 已全部修完**

这一节的所有条目已经实现并验证，留在这里只作记录：

| 项目 | 怎么修的 |
|---|---|
| `lrand48` / `srand48` | 实现了 POSIX 的 48 位 LCG（`a=0x5DEECE66D, c=0xB`，返回 `X'>>17`），和 Vita 版链接 newlib 的行为一致 |
| `qsort` | 实现了，用 `cpu_call_guest()` 重入解释器跑 guest 比较函数；选择排序，元素原地交换 |
| `statfs` | 按 bionic arm 布局填满 64 字节结构体（f_bsize/f_blocks/f_bfree/...），报告 4 GB 可用 |
| `opendir` / `readdir` / `closedir` | 实现了；`struct dirent` 按 **bionic 布局**手工拼装（`d_name` 在 +10，宿主在 +19），不能直接拷贝 |
| `remove` / `unlink` / `rename` / `lstat` | 实现了；失败返回 -1 而不是静默成功 |
| `fsync` | 返回 0（写已经落到宿主的文件里，报失败会让引擎以为存档没成） |
| `raise` | 返回 0（引擎拿它当"不该发生"的断言，真死掉会终止整次运行） |
| JNI `NewObjectArray` | 实现；对象数组是 4 字节句柄数组，每槽预填 `initial` |
| JNI `GetObjectArrayElement` / `SetObjectArrayElement` | 实现；越界写丢弃（真实 JNI 会抛异常，这里没有 Java 接） |
| `CC_MD5` | 返回 16 字节零数组 —— 和 Vita 版 `CC_MD5_stub` 完全一致，不是第二个未验证的 MD5 |

效果：**运行期"被调用但没实现"的 import 从 6 个降到 0 个**
（报告里剩下的两条 `host/File::GetFullPathName` / `host/ParseOffsetAndLengthAndPath`
是被 patch 接管的 guest 函数，本来就有实现，只是不走 `impl` 计数）。JNI 的
worklist 也归零。

新增的 `cpu_call_guest()`（`src/cpu.c`）是这一批里唯一有结构风险的改动，它要
保存/恢复 `cpu_run` 会改的全局（寄存器组、`g_budget`、`g_thumb`、profiler 的
当前函数范围、trace ring 的游标）。**`g_insn_count` 故意不恢复**——它是整次运行的
累计值，嵌套调用必须推进它，否则外层永远发现不了已经做的工作。

### 验证

- `./build/spike game/libgame.so --boot --frames=600 --no-profile`
  → `average over 119 frame(s): 8701 instructions`、`final pc=0badf00c`（基线未变）
- 完整链路 6 个 tap → `GAMESTATE_nState` `0 → 13 → 2`，到达 gameplay
- `tools/difftest.sh --all` → 5904 入口 / 3,951,973 条指令，**全部一致**
- `tools/build.sh browser` 通过

### 仍然没触发过的路径

`opendir`/`readdir`/`remove`/`rename`/`qsort`/`srand48` 在这一轮 6500 帧里
**调用次数为 0** —— 它们是在存档列表、删档和背包排序时才走到的，而那些路径
目前只有从 gameplay 再往后操作才会进入。实现是照着语义写的，没有端到端跑过；
存档往返验证（P0）会顺带覆盖它们。

## P1 — Android 只有"能跑"，没有验收

`docs/ANDROID.md` 自己列的四项都还没做：

- **音频没人听过**（模拟器用 `-no-audio` 跑的）；
- **生命周期**（Home / 旋转 / surface 销毁）只有代码路径，只走过启动；
- **存档**（和浏览器同状态：`ensure_parent_dirs` 修好了，但没做往返验证）；
- **真机性能**（模拟器是 SwiftShader，不算基准）。

## P2 — 测试深度

- 差分测试是**零参数**进入每个函数的，所以 `capped 25` / `bad-mem 18` 那 43 个入口
  只验证了序言，**函数体没被覆盖**。要往函数体里走，得用
  `--stop-at=ADDR --dump-guest=FILE` 在真实调用点拍快照，再用 `oracle.py` 从快照起跑。
- `--watch=ADDR`（观察宿主侧写入，指令 trace 看不到的那一类）还没接进扫描。

## P2 — 性能 / 体验

- **profiler 默认开着**（`g_do_profile = 1`），浏览器和 Android 都没有关闭开关。
  native 实测：开 10.33 s，关 9.34 s，**约 +10.6% 墙钟**。浏览器里解释器占 71%，
  这 10% 是白烧的。一行改动。
- **开机 17 秒**里 LZMA + inflate + PNG 约占全部指令的 37%，而且是一次性成本。
  它们是可移植的 C 库而不是引擎逻辑，可以用现成的 `patches_replace_with_import`
  换成宿主实现（`File::GetFullPathName` 已经是这么做的）。
- **只有想做 60 fps 时**才需要：`-O3 -flto`（现在是 `-O2`）→ 把 `cpu.c` 的逐指令检查
  提到循环外 → 优化 `GRP_*` 软件 blitter。15 fps 下都不需要。
  改 `cpu.c` 之后必须重跑 `tools/difftest.sh`。
- **网页外壳还是 emscripten 的默认页**：有 "powered by emscripten" 横幅、白底、
  下方 `textarea`、滚动条、"Resize canvas" 复选框；canvas 固定 960×544，手机上会溢出。
  另外 `src/platform_emscripten.c` 里 `attr.alpha = 1`，和它上面那段注释
  "alpha = 0, not 1" 直接矛盾 —— 画面是合成在白底上的，淡入时会发白。
  要当成品用，需要自定义 shell（黑底、viewport meta、CSS 自适应、去掉 textarea）。

## P3 — 仓库与文档

- `data/` 下有 **1.6 MB 派生自版权资源的数据被 git 跟踪**
  （`i_pack.dat` 1,364,059 B、`i_tile.dat` 194,928 B、`i_mapfeature.dat` 44,928 B 等），
  而且 `hubcheck.sav` 每跑一次游戏就变脏。按仓库自己的规矩
  （`.gitignore` 里 `game/`、`*.so`、`*.apk` 先例），这些不该提交。
- **文档状态章节全部过期**：`README.md` 的 Status、`docs/PERFORMANCE.md` 的
  "What is left"、`docs/HANDOFF.md` 的 "Where things stand"。
  还写着"音频未接"、"文字不渲染"、"浏览器 trap 在 `gl4es_glLoadIdentity`"、
  "PNG inflate 未工作" —— 现在都不成立，接手的人会被误导。
- 父仓库有未提交的 `.gitignore` 改动和未跟踪的 `WASM_PORT_ASSESSMENT.md`。
- **freestanding wasm 目标**（`tools/build.sh wasm`）堆越界未修。只影响 AOT 吞吐测量路线，
  不影响浏览器 / Android / native 任何产物。
- **VFPv2 未实现**。二进制是软浮点 ABI（无 `Tag_ABI_VFP_args`），实际用不到。

## P3 — 跨平台

现在完整宿主只有 **浏览器** 和 **Android** 两个。Windows / macOS / iOS / Switch
各自需要一个新的窗口 + 音频后端：

- 核心解释器、ELF 加载、JNI 面、gl4es 的指针翻译层都可以直接复用；
- 每平台大致 500–900 行宿主代码（`platform_*.c` 一个 + `sound_*.c` 一个 + 应用外壳）；
- 桌面 native 构建至今**故意没有窗口**，它是调试 / 测量宿主；Linux 上"玩"是走浏览器（`tools/play-linux.sh`）。

---

## 进度（2026-10-21 更新）

- [x] **P1 宿主面的洞** —— 全部实现并验证（见上）。运行期未实现 import 归零。
- [ ] P0 存档 / 读档闭环 —— `SAVE_SaveData` 的路径问题已由 `8949ebb` 修掉，
      但**还没有做过一次真正的存 → 读回验证**。
- [ ] P0 浏览器持久化（IDBFS）
- [ ] P1 Android 四项验收
- [ ] P2 profiler 默认关、开机加速
- [ ] P2 自定义网页外壳
- [ ] P3 文档刷新、`data/` 取消跟踪

## 建议动手顺序

1. **浏览器持久化（IDBFS）+ profiler 默认关掉** —— 两个都是小改动，用户直接可感。
2. **存档端到端** —— 存一次、读回来。这也会顺带覆盖上面那批"写了但没跑过"的
   FS 路径（`opendir`/`readdir`/`remove`）。
3. **Android 四项验收**（音频、生命周期、存档、真机性能）。
4. **自定义网页外壳** + 刷新三处文档状态章节 + `data/` 取消跟踪。

动手前先 `git status`，避免和正在改 `src/main.c` 的 session 撞车。

### 一个踩过的坑，别再踩

用 `TAPS="--tap=... --tap=..."` 然后 `$TAPS` 传参，在 **zsh** 里不会做单词分割，
整串会变成**一个参数**，只有第一个 `--tap` 被解析到。表现是"游戏卡在 Logo 不动"，
看起来像代码回归。跑多 tap 的链路时请直接写字面量，或者用 `${=TAPS}`。
