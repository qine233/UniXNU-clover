# clover 移植轮次报告

本文件是 `sheng-port` 分支的逐轮报告：每轮做完更新一次，读数与判据按轮次倒序排列。
正式的过程记录仍在 `notes/development-log.md`，结构化记录在 `porting.toml`；本文件只保留
「这一轮改了什么、读到了什么、下一轮做什么」。

## 当前状态（2026-11-10 夜）

- 板子：Mi Pad 4（clover，SDM660），串口 `81ffcaaa`；最近一次运行镜像 **r372（v0.7.93）**。
- 主线卡点：**backboardd 自己的线程在向它自己的 IOMobileFramebuffer user client 口做
  send+receive 同步调用，selector 6（swap）间歇性回 `kIOReturnBusy`（0xe00002d5）**，桌面帧率
  被拖到 ≈1 帧/分钟。
- 已排除：缺方法（方法在、也回包）、GPU（不需要，见 §结论）、供电/温度（99%、31.8 °C）。
- 已知副作用：**诊断采样器跑一次后约 18 s 板子会 silent reset**；门控到 `t≥600 s` 可避开早死，
  但每次拿数据仍要赔一靴。

## 逐轮报告（倒序）

### r376/r377 — 面板内容自 t≈125 s 冻结（桌面问题的真实卡点）

- **改动（board 侧，无新 XNU patch）**：
  - r376：每第 128 帧也打 `frame#` 行（payload `929af8c2…`）；
  - r377：新增 `last=<kind>@<t>s:<box>` 进 `dispstate`（payload `1e4f473f…`）。
- **读数（boot 161，整靴 1800 s）**：
  ```
  dispstate ms=1239520 … last=0@125s:305x811..970x1108 … latch=11d copy=11d fc=11d
  dispstate ms=1791590 … last=0@125s:305x811..970x1108 … latch=243 copy=243 fc=243
  ```
  ⇒ **屏幕最后呈现的帧停在 t=125 s**（box 305x811..970x1108，拷贝帧），此后 28 分钟没变；
  同靴客户端仍调 swap 2334 次（593 Busy）、`fc` 涨到 577。
- **判读**：不是「帧率低」，是**面板在 125 s 冻结**。下一支：查 125 s 时 `kickOnce` 的哪个分支停了，
  以及没有 GPU 时 CoreAnimation 是否不再产出新 surface——这决定 GPU 驱动是否为桌面路的必要条件。

### r375 — 关掉采样器：靴活满 1800 s 死线，显示侧第一次拿到整靴数据

- **改动**：采样开火默认关闭（boot arg `clover-bbsample=1` 才开）。patch `<103z>-…-r375.patch`，
  md5 `0c1ac26e…`，payload `767325f2…`，tag **v0.7.96**。
- **读数（boot 159，整靴 1800 s）**：
  - `flight: newest entry t=1805079 ms`＋14 枚 deadline 行 ⇒ **自主跑到死线**；此前 ~170 s/~610 s
    的早死全部来自采样执行；
  - `dispstate ms=1800139 … client=pid86 calls=3078 fails=779 regall=0 regs=0 hot=6:1529/764/e00002d5
    latch=2fd copy=2fd fc=2fd` ⇒ **swap 在 retire：765 帧/1800 s（≈0.43 fps）**，selector 6 恰好
    一半 Busy；
  - `regall=0 regs=0`：没有任何 vsync/端口注册。
- **判读**：显示管线是活的但极慢；50% Busy 的「新提交推进上一帧」形状说明**瓶颈是客户端提交节奏**
  （~2.3 s/次），不是 swap 本身。下一支查「客户端为什么这么慢」与「selector 72/register 为何从未发生」。

### r374 — 帧计数进 remdisp；确认「采样器执行本身致死」

- **改动**：`remdisp` 追加 `fc=`（display+252）。patch `<102z>-…-r374.patch`，md5 `c130254c…`，
  payload `e01bd634…`，tag **v0.7.95**。
- **读数（boot 158）**：没有 `remdisp` 行（采样没抓到 send），最新 flight `t=611232 ms` ⇒
  采样后 ~11 s 复位；`dispstate … pick=0 p58=0 calls=9 fails=6 regall=0 latch=0 copy=0 fc=0`。
- **判读**：boot 156（hops=4，采样后 18 s 死）＋ boot 158（无 send，采样后 11 s 死）⇒
  **采样器执行本身致死**，与数据无关。关键身份（selector 5/6、IOMachPort、
  IOMobileFramebufferUserClient）已拿到 ⇒ **主线转向显示侧修复，不再依赖采样器**。

### r373 — 阻塞拍读显示状态：powered=1，Busy 另有来源

- **改动**：hop 块读 `native_legacy_object+5808` 起 16 字 → `remdisp` 行。patch
  `<101z>-…-r373.patch`，md5 `746a1cc6…`，payload `64f2adcf…`，tag **v0.7.94**。
- **读数（boot 157）**：
  `remdisp t=602 pid=58 nm=630b d=4b0.780.e.d.d.ff000000.0.1.1.0.0.0.0.0.0.0`
  → **+5840（powered）= 1**，`announcePoweredByFirmware()` 生效；
  `dispstate ms=600635 hot=6:23/11/e00002d5 latch=c copy=c fc=c vs=0/0/0`；
  `regall=0 regs=0` 依旧。
- **判读**：「powered 没写 ⇒ 永远 Busy」被排除。Busy 的第二来源指向 **vsync 客户端注册缺失
  （selector 72 从未成功）**：`armVsync()` 武装不了，交换只在新提交时 kick，前一帧未转完就回 Busy。
- **下一支**：读 selector 72 的返回码与 `kickOnce()` 的 `logicalPM/bootMapping/primary/transferred`
  四个门在阻塞拍的值。

### r372 — 门控实验：早死是「第一次采样」，并且第一次拿到 selector 身份

- **改动**：beat 开火条件加 `secs >= 600`（`osfmk/kern/task.c`）。patch
  `<100z>-clover-r372.patch`，md5 `5215758d…`；payload `590f8ea8…`，tag **v0.7.93**。
- **读数（boot 156）**：
  - 门控后活过 600 s（`r308 beat t=540 target=58`），此前 149–155 每靴都在 167–173 s 死 ⇒
    **早死由第一次采样触发**；
  - 第一次采样在 `t=603` 抓到 `r229 … hops=4`，`remkobj … nm=6703 kotype=29
    vp=fffffff0071cf878`；宿主解码 `__ZTV10IOMachPort + 0x10`；
  - 采样后约 18 s 卡写停在 `t=621886 ms` ⇒ **采样路径本身致死**，数据已落盘；
  - `remcall id=b31 sz=55c`、`rembody w=0.1.5.0.508.ab55fe30.2.ab561376`、
    `remtrap op=200000003`。
- **对表**：`client#3 pid=58 class=IOMobileFramebufferUserClient`；`first selector=5 args=1288/0
  status=0`，`1288 = 0x508` ⇒ 1372 字节请求 = selector 5 + 1288 in-band，**成功**。
- **判读**：W3-A/C 成立；「缺方法」假设被 selector 5 的成功读数推翻一部分。

### r371 — 修采样器的任务错配

- **改动**：`clover_bb_sample()` 在 `proc_find` 后加 `if (task_pid(sb_task) != pid) sb_task = task;`，
  并把 `clover_bb_note_mark()` 移到其后。patch `<99z>-…-r371.patch`，md5 `9f9fc161…`，tag **v0.7.92**。
- **读数（boot 155）**：仍在 169 s 死（`r229 t=181 hops=0`）⇒ **任务错配不是死因**（V1-B）。

### r370 — `remkobj` 加 `vp=`（kobject 的 vptr）

- **改动**：kotype 29/30 时在端口锁内读 kobject 第一个字，行加 `vp=`。patch `<98z>-…-r370.patch`，
  md5 `5865bd4a…`，tag **v0.7.91**。
- **读数（boot 153）**：无 remkobj 行（死得早）；**boot 156 才出真行**（见 r372）。

### r369 — 回退 r368 的局部 target，改为不清 `clover_bb_target_pid`

- **改动**：恢复 `if (clover_bb_target_pid != 0) clover_bb_sample(...)`；generation-change 不再清
  目标 pid。patch `<97z>-…-r369.patch`，md5 `74541d23…`，tag **v0.7.90**。
- **读数（boot 151）**：仍在 172 s 死 ⇒ 早死与 r368/r369 无关（后来 boot 152 的 r367 对照也死）。

### r368 — 局部 target（后被 r369 回退）

- **改动**：beat 用局部 `fire_pid`，为 0 时 `proc_find(58)` 重解析。patch `<96z>-…-r368.patch`，
  md5 `1c3b0b8f…`，tag **v0.7.89**。
- **读数（boot 149/150）**：167/171 s 死 ⇒ 当时的「疑 r368」后来被证明是共性早死。

### r367 — 默认采样 backboardd 自己

- **改动**：beat 发现 `clover_bb_target_pid == 0` 时用 `proc_find(58)` 直接采 backboardd。
  patch `<95z>-…-r367.patch`，md5 `157603db…`，tag **v0.7.88**。
- **读数（boot 148）**：1805 s；`r308 beat t=180 target=58`、`r229 t=180 hops=0` ⇒ 闩锁生效，
  但只开了一次（generation-change 清 0）。

### r366 — 读请求体 `rembody`

- **改动**：`hdr[0] & MACH_SEND_MSG` 时读 `p+24` 起 8 字。patch `<94z>-…-r366.patch`，
  md5 `3019d5f3…`，tag **v0.7.87**。
- **读数（boot 147）**：整靴 `target=0 ent=0`（没有默认闩锁，burst 未开火）。

### r365 — `remtrap` 读 trap 寄存器

- **改动**：hop 行加 `op=`/`x4=`/`x6=`/`x7=`。patch `<93z>-…-r365.patch`，md5 `38fcf563…`，
  tag **v0.7.86**。
- **读数（boot 146）**：两个 hop 都是端口集合、`asked=0`，X 不判。

### r364 — `remwin` 两窗读数

- **改动**：读 `p-16` 与 `p+32`。patch `<92z>-…-r364.patch`，md5 `5611c885…`，tag **v0.7.85**。
- **读数（boot 145）**：**第一次读到真 mach 头**：`b=1513 sz=55c rem=6f0f loc=6b07 id=b31
  m0=0 m1=1` ⇒ backboardd 在向 IOKit 连接口发 1372 字节同步请求。

## 结论与方向

1. **不是 GPU 问题**：阻塞点是 IOMobileFramebuffer user client 的 swap，回 `kIOReturnBusy`；
   GPU 驱动不会改变这个回包。
2. **不是「缺方法」**：selector 5（1288 in-band）成功；selector 6（swap）时好时坏（Busy 占一半）。
3. **主攻方向**：让 legacy swap 不再回 Busy——读 `bbswap` 的 `pw/ps` 与 `transferred` 队列状态，
   钉死 Busy 的产生条件；驱动注释已点名「不设 powered 状态则 selector 6 永远 Busy」，我们确实在
   `announcePoweredByFirmware()` 里做了，但 Busy 仍出现，说明还有第二来源。
4. **副作用**：采样器跑完后 ~18 s 复位——先用 `t≥600` 门控拿数据，随后按
   `proc_find → 锁 → 线程走表 → copyinmap` 逐段二分。

## 下一轮（r375）

- 显示侧（不再用采样器）：在 `wrappedRegister`/`wrappedExternal` 里记录 selector 72 与 register
  的返回码、`kickOnce()` 的 `logicalPM/bootMapping/primary/transferred` 四个门；
- 若 selector 72 从未被调用 ⇒ 查 backboardd 为何不做 vsync 注册（可能要先让 selector 6 成功返回）；
- 采样器致死作为独立问题排期（分段门控：proc_find → 锁 → 线程走表 → copyinmap）。
