# PMIC 轨（SPMI PMIC Arbiter）读数路线（排期 6 底稿）

这份文件的用途只有一个：把「问一颗轨现在什么状态」这件事，从一块板子的地址表变成一组有出处、
可反驳、可重放的读数。所有偏移都从**本板自己的 DT** 和**两份彼此独立的 arbiter 驱动**里取；
sheng（SM8550，arbiter v7）能借的是分段方式和事务形状，不是任何一个数字。

对应驱动：`drivers/CloverPower`（`PowerRegs.h` 是地板图，`PowerArb.h` 是被宿主模型驱动的那半）。

## 1. 取证办法（先说这一步，因为它决定后面每条读数引用的是谁）

两份 arbiter 驱动都**归档进仓库**，不再引用 `/tmp`：

```
notes/sources/sdm660/spmi-pmic-arb.c           1,382 行  sha256 bedee16415c7290c2c07414623edc8af16b185f475e3512bb963e7c67076754c
notes/sources/sdm660/spmi-pmic-arb-mainline.c  2,182 行  sha256 52e78067daedd326af49c09033932c84bdcdd8a1a9df77e4fc7ae12d18cf153b
```

| 记号 | 是谁 | 为什么用它 |
| --- | --- | --- |
| VEND | LineageOS `android_kernel_xiaomi_sdm660` @ lineage-20 的 `drivers/spmi/spmi-pmic-arb.c` | 本板出货产品今天跑的这颗 arbiter 驱动，它的 ops 表就是硬件被配置成的样子 |
| MAIN | `torvalds/linux` master 的同名文件 | 独立第二棵树：版本下界、owner 表、revid 判据与 VEND 逐条一致，**v3 那张表两边都有** |
| DT | 烧进 `~/mu-clover/Resources/DTBs/clover.dtb` 的那颗转译本（`scripts/dump-clover-dtb-dts.sh` 出双 sha256，r312 已量：DTB `815b82556e417f…`、转译本 `df8ef4a7759a81f…`） | 五扇窗的地址、大小、名字，`qcom,ee`，两颗 PMIC 的 sid，两条轨的名字与电压范围 |
| SHENG | `~/UniXNU-sheng/package/BootResetCore.h` | 同一块 arbiter 在 SM8550（v7）上**已被真机证明**的事务形状：写 CMD、轮 STATUS、取 RDATA0/1 |

一条规矩：`v3` 这一档不是我们从「读数像 v3」推出来的，是 VEND:1219-1222 与 MAIN:2102-2110 两条
else-chain 各自独立写出来的。两份独立出处这件事，正是 r313 敢把「拒绝对这张地图做任何解释」
换成「按版本字装表」的全部依据。

## 2. 已钉死的设备事实

| 事实 | 值 | 来源 |
| --- | --- | --- |
| 仲裁器节点 | `qcom,spmi@800f000`，`compatible = "qcom,spmi-pmic-arb"` | 转译本 :2265 |
| 五扇窗 | core `0x800f000+0x1000`、chnls `0x8400000+0x1000000`、obsrvr `0x9400000+0x1000000`、intr `0xa400000+0x220000`、cnfg `0x800a000+0x3000` | 转译本 :2267-2268，另与 `/proc/iomem` 逐段吻合（三源无分歧） |
| EE / channel | `qcom,ee = <0x00>`、`qcom,channel = <0x00>` | 转译本 :2270-2271；VEND:1266-1291 两者都拒绝 >5 |
| 中断 | `interrupts = <0x00 0x146 0x00>`，SPI 326 ⇒ INTID 358 | 转译本 :2272（round183 那帧照片把末位读成 350，本轮起不再靠照片） |
| 两颗 PMIC | `qcom,pm660@0`（sid 0，:2283）、`qcom,pm660l@2`（sid 2，:2852），revid 都在 `0x100` | 转译本 + MAIN `qcom-spmi-pmic.c:19-24` |
| 面板要的两条轨 | `pm660_l11`＝vcc_i2c（PPID `0x04a`，1.78–1.95 V）、`pm660l_l3`＝vdd（PPID `0x242`，固定 3.0 V） | 转译本 :5236 / :5539；两个 DT 触摸节点（focaltech@38、gt9xx@5d）共用同一对 |
| 时钟 | 仲裁器节点**不声明任何时钟**，VEND 的 probe 也不 request | 转译本 + VEND `pmic_arb_probe()`；所以 mode 1 用「读 core 看答不答」检验这个前提，不自编 GCC 序列 |

## 3. round183 那一帧说的是什么，为什么它不能靠再看一眼照片解决

v0.5.40 实机那一帧（`notes/development-log.md` :13845-13881）里，同一个变量——core 偏移 0 的那个
`%08x`——被打在两行里，而照片给出的转写是**两个不同的值**：

```
clover r182: core=03000000 ee=0 chan=0 intid=350 | cnfg700=00000000 obs00=00000100 intr00=00000000
clover r182: scan pm660-revid ppid=0x001 rc=1(version) ver=30000000 words=0 mapped=0 span=0
clover r182: not a v5 arbiter (03000000); stopping, the map layout is unknown
```

这两个候选不是「差一个零」的小事：`0x03000000` 低于 `V2_MIN (0x20010000)`，落在 v1，而 v1 的
`rd_base`/`wr_base` 都在 core（VEND:1212-1216），本板 DT 却明明声明了 chnls/obsrvr/intr 三扇独立窗；
`0x30000000` 正落在 v3 区间，那一档的地板图 VEND 已经逐条写全。两种读法会把后面每一条偏移都带进
「自信地错」。而**再看照片本身不可能裁决它**——照片已经给过两个答案了，缺的从来不是清晰度。

所以 r313 改的是仪器形状，不是再拍一张：

1. 同一个字**四条腿**打印：`hex=`、`dec=`、逐半字 `nib=x-x-…-x`、以及第二次读的 `again=`；
2. `stable=` 当场判两读是否相等，不等就 `version moved under us` 停住；
3. `band=` 打印装上去的那张表，并把它两侧的 `floor=`/`next=` 一起印出来，读者可以自己重跑那条分支；
4. 落在任何已知表之外时打印 `band=none floors v2=… v6=…`，而不是一个含糊的「不是 v5」；
5. 一条**不经这张表**的对照：`core+0004` 同一次读、两种解码（VEND 的 `INT_EN` 与 MAIN 的
   `FEATURES` APID 计数），再加 `differs=`——它证明 core 窗确实在按位应答，而不是把上次总线残留
   当读数（这一格按 memory「仪器要能看见它问的那一刻」的第一/十二层：判决行必须有一条不经被判决
   之物produce 的副本，且判据行得有正对照）。

还有一条更根本的：**本轮起这一格不再靠拍照。** `clover r` 在 CloverSdhci 的 tee 白名单里，
r313 单独申请了 `r313-power` / `r182-power` 两枚类配额（各 48 行，一次性 PMGR 尾巴突发，走
`reserved` 豁免链），读回是卡上文本，前导零不再是眼睛的活。

## 4. 版本字决定地板：四处分歧，和三件不动的事

分支本身照 VEND:1212-1224 / MAIN:2102-2110 逐支搬：

| 区间 | 档 | 本端口怎么做 |
| --- | --- | --- |
| `< 0x20010000` | v1 | 返回 null，打印 `band=none`（DT 声明了五扇窗而 v1 没有 obsrvr/chnls 之分，这张表我们没资格猜） |
| `[0x20010000, 0x30000000)` | v2 | `pa_layout_v2` |
| `[0x30000000, 0x50000000)` | **v3** | `pa_layout_v3`（r313 新增，r182 在这一档是拒绝解释） |
| `[0x50000000, 0x60000000)` | v5 | `pa_layout_v5` |
| `≥ 0x60000000` | v6/v7 | 返回 null——**这是一处有意与厂商的分歧**，见 §6 |

四处分歧（VEND 的 ops 表逐条抄来，行号见括号）：

| 东西 | v2/v3 | v5 |
| --- | --- | --- |
| APID 地图基址 / 条目数 | `core + 0x800 + 4n`，512 条（:1108-1111，:910-911 的界限） | `core + 0x900 + 4n`，448 条（:1113-1116） |
| observer 通道 | `obsrvr + 0x1000*ee + 0x8000*apid`（:983） | `obsrvr + 0x10000*ee + 0x80*apid`（:1005） |
| `ACC_ENABLE`/`IRQ_STATUS`/`IRQ_CLEAR` | 在 **intr** 里：`+0x0/+0x4/+0x8`，步幅 `0x1000*apid`（:1060-1064、:1078-1082、:1096-1100） | 在 **chnls** 里：`+0x100/+0x104/+0x108`，步幅 `0x10000*apid`（:1066-1070、:1084-1088、:1102-1106） |
| owner ACC_STATUS 基址 | `intr + 0x100000`（v2，:1036-1040）／`+ 0x200000`（v3，:1042-1046），步幅 `0x1000*ee` | `intr + 0x10000*ee`（:1048-1052） |
| 映射范围（本端口的推论） | obsrvr 与 intr **按 DT 声明整扇映**（VEND:1227-1236 就是整段 `devm_ioremap_resource`）：`0x8000` 的 APID 步幅下，64 KiB 只够 APID 0 和半个 APID 1 | 一块 EE 窗（`0x10000`）＋一页 intr：`0x80` 步幅下 512 通道装得下 448 个 APID |

不动的三件——这才是「换了档，值得有的答案还在」的理由：

- 归属表在每一档都是 `cnfg + 0x700 + 4n`、取 3 位（VEND:50-51；v2/v3 的惰性走查在 :858-861，v5 的地图读法在 :920-922）；
- 通道内命令块在每一档都是 CMD `0x00` / STATUS `0x08` / RDATA0 `0x18` / RDATA1 `0x1C`（VEND:27-33）；
- 地图字里 PPID 的取法（`(w >> 8) & 0xfff`，:867 与 :917）和命令编码 `fmt_cmd_v2`（:1025-1028，v5 也装它）在 v2 往上完全一致。

## 5. 本轮抓到并修掉的两个静默错位

这两个都不是「读数难解释」，是**读错了地方还打印成一个名字**——只有把地址和产生它的规则一起印出来
才会暴露，所以两条都改了报告形状。

1. **owner ACC_STATUS 按 APID 索引**（r182 的写法）。厂商按 **32 个一组的组号**索引：
   VEND:522-523 走 `first = min_apid >> 5` 到 `last = max_apid >> 5`，:542 再 `apid = id + i * 32`
   还原。按 APID 乘 4 只在 APID 0 与 1 上和厂商地址重合，往上就读到隔壁那一组的字。宿主模型现在
   同时断言两种口径（`pa_owner_acc_addr(v5,0,40) == pa_owner_acc_addr(v5,0,41) == 4`，而 64 ⇒ 8）。
2. **在 v2/v3 下把 `chnls` 偏移当成 access-port enable 打印**。那三个字在 v2/v3 归 `intr`，而 intr
   窗上面已经为 IRQ 映好了；再映一页 chnls 去读、然后按 enable 的名字印出来，等于给一个事务寄存器
   偏移改了个名。r313 的做法是：这一档**不映 chnls**，并当场打一行
   `acc words are intr offsets on v3; no chnls page mapped`——缺席要说得出为什么缺席。

顺带：IRQ 归属本身也是随档变的规则（v5 看地图字 bit 24，:46/:918/:924；v2/v3 没有这个位，
VEND:860-861 直接 `irq_ee = write_ee`）。所以 `irq_owner=` 这个布尔位被拆成 `irq_ee=` + 
`irq_src=map_bit24|cnfg_owner|unknown`——列名和填它的表达式同源（memory 第十一层）。

## 6. 两表混读的方向性，以及为什么版本字是唯一防线

这条不是推的，是宿主模型跑出来的（`tests/clover_power_host.cpp` case 3，`scripts/test-clover-power.py`
是 `check-clover.py` 的一道闸）。两张表**不对称**：

- v2/v3 的地图从 `0x800` 起、一路铺到 core 窗尾，**包含** v5 的 `0x900..0xfff`。于是拿 v3 表去走一张
  v5 形状的地图：每个 PPID 都还找得到，但 APID 号**恰好高 64**（`(0x900-0x800)/4`），而归属与 IRQ 规则
  都拿这个被移位的 APID 去索引，重赛点（tie-break）的形状跟着一起变——**全程没有一次报错**。模型里
  pm660_l11 在 v3 表下报 `candidates=3`、选中 APID `9+64`，而 v5 表自己的答案是 7。
- 反过来会拒：v5 的走查起点在 v3 地图写过的字之上，读到全零，报 `PA_E_NOENTRY`。

结论写进 `PowerArb.h`：`pa_layout_for()` 之后没有任何一道检查能抓到「档读错了但居然答了」这一支，
**唯一防线是版本字**——这就是它值四条腿打印的原因。

还有一处与厂商的有意分歧也写进同一处：VEND:1222-1224 的 else 分支**没有上界**（`else … = &pmic_arb_v5;`），
MAIN:2102-2110 同样形状，也就是说厂商自己会把一颗 v6/v7 仲裁器用 v5 表来驱动。本端口在
`V6_MIN (0x60000000)` 停住返回 null：SHENG 的 v7 家族是 `0x2000`/`0x00` 基址加 `0x20`/`0x8000` 步幅
（SHENG:22-23），一个未知版本字的正确答案是 `PA_E_VERSION` 加上把那个字印出来，不是一次看着合理的读。
将来真有板子落在那一档，修法是加第四行，不是放宽上界。

## 7. 这一轮解开的下游格子（为什么值得为一张地址表动手术）

「只写了下一档的地址」这件事挡住的不是一个读数，是一串：

- 排期 4 的第二闸门（XNU 到底能不能自己开一颗轨）——答案只在 `cnfg + 0x700 + 4*apid` 里，而 r182
  在报出那个数之前就停了；
- round181 那句 `rails unproven`：面板两条轨（`pm660_l11` / `pm660l_l3`）的 enable 位 `0x46` 没读到，
  「芯片是死的」和「没人给它上电」就还是同一个观察；
- GPU 线的第二条轨腿（`pm660l_s3`）与 Wi-Fi 那四条（`pm660_l5/l6/l9/l19`）——同一条门后面的同一张表。

## 8. 预登记的判读分支（这一枚镜像刷下去时读什么）

发版前写死，避免读数回来后现编解释。前提是 `clover-power=1`（本轮出厂的这一档：不写一个字节、
不起一次事务）。

甲：`hex=30000000` 四条腿一致、`stable=1`、`band=v3` ⇒ 档定了。接下来看
`layout=v3 map=0x800+4 x512 entries=512 …` 之后的 `mapped=`：`mapped>0` 才是「boot 留下了 APID 表」，
此时排期 4 的闸门行（`write-owner is EE n, this EE is 0: reads ok, writes denied`）第一次有资格当结论。

乙：`band=v3` 而 `mapped=0` ⇒ 地图是空的，这跟「我们读错了档」是两件事。这一支把问题交回引导加载器
（谁都没配过这张表——GPU 线 boot 81 的 13 个 LDO 词里除两枚外全零，已经是同一种形状的证据）。

丙：`stable=0` 或出现 `version moved under us: chose=… scan=…` ⇒ 版本字在两读之间变了。**这一支里本轮
所有偏移读数一并作废**，不许挑其中一次读当结论；下一步是查 core 窗的映射属性，不是查 PMIC。

丁：`hex=03000000` ⇒ `band=none`，落在 v1。那要解释的是矛盾本身：DT 声明了五扇独立窗而 v1 不分
obsrvr/chnls。这一支不改代码，是把「v1 的 rd_base==wr_base==core 与本板 DT 能同时成立吗」写成问题。

戊：`differs=1`（`vend_int_en` 与 `main_apids` 两种解码对不上）且 `mapped>0` ⇒ 不是仪器坏了，是本板
这颗硅对 `core+0x0004` 的意思只支持两种读法之一；地图走查仍按 core 窗大小定界（VEND 的做法），
这一支不改走查界限，只把「哪读法成立」记成一条新事实。

己：档定了、`mapped>0`、revid 那五行报 `burst no, singles yes` ⇒  arbiter 只答单字节。这一支的代价要
现在记账：后面每一次多字节读（含电压 `0x40/0x41` 那两字节）都得改成两次单读，别把 burst 当默认。

庚：整帧里没有 `clover r313:` 行 ⇒ **不许读成「 arbiter 没答」**。先按 memory 第十三层数饥饿：
`r313-power` 是 `reserved` 豁免链上的一次性突发，先看 tee 自己的 `calls=`/first-drop 自报行，再谈设备。

## 9. 排好的下一格（按「不需要 RPM 也能读」排序）

1. 上机读回甲/乙/丙三支中的一支（boot 103 的现场在 `#622`，这一枚排在它后面）；
2. 若 `band=v3` 且 `mapped>0`：把 `clover-power` 升到 2，只多问两颗 PMIC 的 revid（一次 5 字节突发，
   仍不写任何字节），把「 Peripheral 是不是 DT 说的那颗」结掉；
3. 再升 3，读两条轨的 `0x46` enable 与 `0x40/0x41`，回答 round181 欠的那一句；
4. 闸门行真是 `writes denied` 才谈 RPM——那是排期 6 的另一格，不在本底稿的承诺里。

不写的东西：不改时钟（节点没声明、两棵树都不 request）、不写 voltage、不写 enable、不写 mode、
不碰 pull-down / soft-start、不发 RPM 协议、不给没证明的子型编微伏数。
