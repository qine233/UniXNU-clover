# Wi-Fi（WCN3990 / `qcom,icnss`）传输选型（并行线底稿）

这份文件的用途和 `notes/gpu-bringup.md` 一样：把一条并行线从「计划」变成一组能按顺序验证的硬件事实。
所有地址都从 clover 自己烧进卡的那颗 DTB 里取，不从邻近 SoC 的 dtsi 类比；厂商驱动一律归档并带 sha256。

**一句话结论（r312，2026-11-04）：本板的无线不是「寄存器没时钟」那一类问题，而是「子系统固件 + QMI 服务」
那一类问题，而且 iOS 侧没有任何驱动能绑这颗芯片。所以 Wi-Fi 这条线停在只读侦察，不再往里投驱动工时；
省下来的工时给桌面那一格的「画面本身」。** 下面每一句都是可以在盘上重放的。

## 1. 取证办法（先说这一步，因为它决定后面每条读数引用的是谁）

DT 的读数来自**我们烧的那颗 DTB**，不是来自 `~/UniXNU-clover/notes/sources/sdm660/*.dtsi`（那些是
上游/厂商的源码，本板出厂 DTB 会把它们改过）。转译是确定性的一步，已经收进脚本，任何人可以重放并
核对同一枚 sha256：

```sh
scripts/dump-clover-dtb-dts.sh
# DTB   sha256 815b82556e417fa826060d262e815ae94a5d8c55283026b6b4bed1c502b673b8
# 转译本 sha256 df8ef4a7759a81f240dcb68649a6a35b770b858beba8555220726aa57198603b（16,612 行）
```

下面所有行号都是这份转译本的行号。**phandle 的解析也在同一份文件里做**（找 `phandle = <0x…>;`
那一行，往上读它所属的节点），不靠另一份 dtsi 的名字——本板的节点名和上游不一样。

## 2. 已钉死的设备事实

### 2.1 无线那颗芯片在 DT 里长这样（转译本 :5833-5852，逐字）

```
qcom,icnss@18800000 {
    compatible = "qcom,icnss";
    reg = <0x18800000 0x800000 0xa0000000 0x10000000 0xb0000000 0x10000>;
    reg-names = "membase", "smmu_iova_base", "smmu_iova_ipa";
    iommus = <0xe9 0x1a00 0xe9 0x1a01>;
    clocks = <0x26 0x14>;
    clock-names = "cxo_ref_clk_pin";
    interrupts = <... 0x19d..0x1a9 ...>;      /* 12 条，SPI 413..425（419 缺席） */
    vdd-0.8-cx-mx-supply = <0xf6>;
    vdd-1.8-xo-supply    = <0xf7>;
    vdd-1.3-rfa-supply   = <0xf8>;
    vdd-3.3-ch0-supply   = <0xf9>;
    qcom,vdd-0.8-cx-mx-config = <0xcf080 0xcf080>;   /* 848000 / 848000 uV */
    qcom,vdd-1.8-xo-config    = <0x1ab3f0 0x1cfde0>; /* 1750000 / 1900000 */
    qcom,vdd-1.3-rfa-config   = <0x124f80 0x14e790>; /* 1200000 / 1370000 */
    qcom,vdd-3.3-ch0-config   = <0x30d400 0x33e140>; /* 3200000 / 3400000 */
    qcom,wlan-msa-memory = <0x100000>;
    qcom,wlan-msa-fixed-region = <0xfa>;
    qcom,smmu-s1-bypass;
};
```

| 引用 | 解析到（同一文件里的原节点） | 关键点 |
| --- | --- | --- |
| `0xf6` | `:5111` `regulator-name = "pm660_l5"`，`compatible = "qcom,rpm-smd-regulator"` | min 525000 / max 950000 |
| `0xf7` | `:5209` `pm660_l9_pin_ctrl`，同 compatible | `qcom,enable-with-pin-ctrl = <0x00 0x02>`、`qcom,init-pin-ctrl-mode = <0x02>` |
| `0xf8` | `:5142` `pm660_l6_pin_ctrl`，同 compatible | 同上 |
| `0xf9` | `:5368` `pm660_l19_pin_ctrl`，同 compatible | 同上 |
| `0xfa` | `:16441` `wlan_msa_mem@85700000`，`removed-dma-pool`，1 MiB（旁边还有 `wlan_msa_guard@85600000` 1 MiB） | 芯片的 MSA 内存池，`no-map` |
| `0x26` | `:4305` `qcom,rpmcc { compatible = "qcom,rpmcc-sdm660", "qcom,rpmcc"; }` | 时钟由 **RPM** 持有 |
| `0xe9` | `:8912` ANOC2 SMMU（`clocks = <0x26 0x59>` = `smmu_aggr2_noc_clk`） | SMMU 自己的聚合时钟**也是 RPMCC** |

三件事一次读出来：

1. **没有 PCIe。** 整份转译本里 `pcie` 只出现在一枚 reg-names 字符串（`:2143` 的 `pcie0-hwev`，
   那是 STM 触发域的名字，不是主控制器节点）。本板 DT 里**没有一颗 PCIe 主控制器节点**，也没有
   `pci` 节点。所以「先枚举 PCIe 再谈无线」这条路在 clover 上不是难，是**不存在**。
2. **芯片是 MMIO 可见的**：`membase = 0x18800000 + 0x800000`（8 MiB）。
3. **它每一枚依赖都在 RPM 手里**：四颗轨（rpm-smd-regulator）、RF 参考时钟（rpmcc）、
   连给它做地址翻译的 SMMU 的聚合时钟也是 rpmcc。其中三颗轨还带 `enable-with-pin-ctrl`，
   即「使能跟随 HW_EN1 引脚」——那既不是 AP 的一条寄存器写，也不是我们能在 SPMI 上改的位。

### 2.2 厂商驱动自己怎么说（归档件，别凭印象引用）

`notes/sources/sdm660/icnss.c`（sha256 `1c7cfa6d9a8d573d014b901fa9da88b6d94ca628f8e8304188f4d0101f2d70b6`，
4,121 行，`LineageOS/android_kernel_xiaomi_sdm660` @ `lineage-20`，`drivers/soc/qcom/icnss.c`）与
`notes/sources/sdm660/icnss.h`（sha256 `e1e430b820d84da16e72f232aba17a913c5d8c1d5ee8d329caac2cd6a1dba7ff`）：

* **全文 `pci` 出现 0 次。** 「无线 = PCIe 设备」这条假设在这颗驱动里不成立，和 2.1 里 DT 没有 PCIe 节点
  正好对上——两份独立证据说同一件事。
* `:3680` 拿 `membase`，`:3688` `devm_ioremap`；`:2256` 把这块虚拟地址交给 `info->v_addr`，
  也就是**只当 MSA/内存池用**。整个文件里 `mem_base_va` 只有 4 处（`:2256`、`:3688`、`:3690`、`:3697`），
  **没有一处按寄存器偏移去 `readl`/`writel` 它**。⇒ 不存在「照着 GPU 那条线，读一枚 chipid 就当芯片在」的入口。
* `:574 icnss_hw_power_on()` 的正文只有两步：`icnss_vreg_on()` + `icnss_clk_init()`。
  也就是说厂商眼里的「上电」= 把四颗轨抬到 config 里的 µV + 拿到那颗 RPMCC 时钟，**没有任何 MMIO 编程序列**。
* 真正的通信通道是 QMI/service-locator：`:1061 icnss_driver_event_server_arrive()`、
  `:1322 icnss_driver_event_register_driver()`，头文件 `:36/:43` 引 `linux/soc/qcom/qmi.h` 与
  `soc/qcom/service-locator.h`。⇒ 芯片侧跑的是一个固件镜像（Q6/ADSP 家族），AP 这边靠服务发现握手。

## 3. iOS 侧的上限（这条线的天花板，也是它为什么停在侦察）

`~/UniXNU/out/ipsw/21H461/kernelcache.decompressed`（iPadOS 17.7.11 出厂预链接内核，44 MiB，本机在盘）里
与无线相关的类名只有一族：

```
AppleBCMWLAN / AppleBCMWLANBusInterfacePCIe / AppleBCMWLANIO80211APSTAInterface
AppleBCMWLANSkywalkInterface / AppleBCMWLANUserClient / IO80211Controller / IO80211Interface
```

没有高通无线的 NUB、没有 QMI/SLINK 客户端、没有任何能绑 `qcom,icnss` 的匹配项；而它唯一那条 PCIe 路径
需要的主控制器节点本板 DT 又没有（2.1 第 1 条）。**结论：即便 Wi-Fi 芯片上电、固件跑起来、QMI 握手成功，
iOS 这一侧也没有消费者。** 这条线的价值上限是把「无线区现在是什么状态」读清楚，不是把网络做出来。

## 4. 与另外两条线共用的一格债（这条最容易被误读，写清楚）

触摸（`drivers/CloverInput`）与无线用的是同一类轨：`vdd-supply = <0x187>` = **pm660l_l3**（`:5553`，
固定 3,000,000 µV）、`vcc_i2c-supply = <0x17b>` = **pm660_l11**（`:5250`，`0x1b2920`..`0x1dc130` = 1,780,000..1,950,000 µV），
两者 `compatible` 也都是 `qcom,rpm-smd-regulator`。全份 DT 里 `pm8950` 出现 **0 次**（`pm660` 62 次、
`pm660l` 32 次）⇒ `drivers/CloverInput/CloverInput.cpp` 里「PM8950 rails through SPMI」那句是错的，
本轮就地改正（注释，不改行为）。

而「那就用 CloverPower 的 SPMI 通路读 enable 位」这条捷径**在板上已经死过一次**：r182 的 mode 3 实测
`core=03000000`，低于 v5 下界 `0x50000000`，驱动按预登记的判据在解码之前停下（round183 的读数，逐行转录在
`notes/development-log.md:13842-13851`，最后一行就是 `clover r182: not a v5 arbiter (03000000); stopping, the map layout is unknown`）。
同一帧上面那行 `clover r181: no address ACKed; rails unproven` 已经把「轨没被证明」写在进行时的读数里。
⇒ **这两颗轨的 enable 到今天一次都没读到**，「触摸 NACK 是因为没人给它上电」既没证实也没排除。
任何把「轨可能没开」当已知前提的下一步设计都要先补这格读数。

## 5. 排好的下一格（按「不需要 RPM 也能读」排序）

1. **arbiter 的版本那一格先弄清**（纯读数，不新增窗口）：`core=03000000` 到底是「本板 PA 不是 v5」还是
   「读错了窗口/时序」。这决定 4 里那两颗轨能不能读、以及 GPU 线的 S3/S5 电平有没有第二条腿。
   厂商侧对照件已在本仓库（`notes/sources/sdm660/`），mainline 的 `qcom,spmi-pmic-arb` 布局差异要先归档再判。
2. **`wlan_msa_mem@85700000` / `wlan_msa_guard@85600000` 是不是还像「reserved」那样留着**——纯内存图问题，
   不需要 RPM 也不需要芯片上电；但它只回答「引导加载器有没有给无线留过地」，不回答芯片在不在。
3. **`membase 0x18800000` 的读**排在最后，并且要按 GPU 线 r282 的规矩写判据：这块页的时钟在 RPMCC 手里，
   我们从没证明过它在跑；没有正对照的 0 与 `0xffffffff` 都不算读数（见 [[feedback-instrument-horizon]]）。
   真要说「芯片在」，需要的是 2.2 那条 QMI 握手，不是这页的一个字。

**本轮不做的事**：不写轨、不动 RPM 协议、不碰 pin-ctrl 的 HW_EN1、不新建 `drivers/CloverWifi`。
桌面仍 CPU-first（用户定），Wi-Fi 这条线不占主线工时。
