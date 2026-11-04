# 触摸 / QUP v2 I2C（排期 4 底稿）

这份文件只有一个用途：把触摸这一支从「已知不应答」往下切，并且让每一句都能指到**还在盘上的文件**。
round181 把寄存器与 DT 事实钉进 `drivers/CloverInput/InputRegs.h`，round318 把判据的**算术**钉进
`drivers/CloverInput/InputCore.h`，而两边引用的厂商与主干源码，本轮第一次真正落到
`notes/sources/sdm660/`——在此之前 `InputRegs.h` 按行号引用的那些文件**盘上一枚都没有**，
所以任何一句「厂商是这么做的」都没法复查。

## 1. 本轮归档的源码（sha256 当场重数）

仓库与 commit 取自 `board.toml` 的两条 feed 记录，与 `InputRegs.h` 头部三段来源说明一致。
命名规矩沿用 `spmi-pmic-arb.c` / `spmi-pmic-arb-mainline.c` 那一式：**厂商不带后缀，主干带
`-mainline`**。

| 文件（`notes/sources/sdm660/`） | 出处 | sha256 |
| --- | --- | --- |
| `i2c-msm-v2.c` | `E7G/clover-sdm660-ksb` @ `5c81a3faf37e…`，`drivers/i2c/busses/` | `6f35a2e9ce61f7cff4212446d4b607c17030e9dfb5c16c70fa223d3622ce65c3` |
| `i2c-msm-v2.h` | 同上，`include/linux/i2c-msm-v2.h`（`#include <linux/…>`，不在 `drivers/` 下） | `79aca48e1c695d01387216498ef91be3da838c82007697f1cfe2a396f04beeb5` |
| `gt9xx.c` | 同上，`drivers/input/touchscreen/gt9xx_zql6019/` | `fa7d04cd4b20c9b1b0e1f69d57a47a1186c9f3711b3cff6b57256e62eaaf07f2` |
| `gt9xx.h` | 同上 | `48d9ef1f4de81e996b2bf28c7e5d6f625b5b93151dc65df38d45aaa55949fda3` |
| `edt-ft5x06.c` | 同上，`drivers/input/touchscreen/` | `c99bc1cc66a8d33051ce81ebe318e0204cac08348c4758613717762124dcf7b7` |
| `i2c-qup-mainline.c` | `sdm660-mainline/linux` @ `dd1ed52c8ceef…`，`drivers/i2c/busses/` | `994f3308f5559a9eeed8c2832c1d6b15fe5e2c42c080e6e2995d7448de72e241` |
| `edt-ft5x06-mainline.c` | 同上，`drivers/input/touchscreen/` | `4da4146ed7040906f9c49c3b31e91c4f3cfb874fff182cd17c66f7f0103a524f` |
| `pinctrl-msm-mainline.c` | 同上，`drivers/pinctrl/qcom/` | `d6638c03c09f5c0b24d9e343e19fb04ad7d483ec4f1c5fdabde4efc3570d59f8` |

## 2. 线码：我们放上的字节，逐枚对得上主干

`i2c-qup-mainline.c:107-112` 给出 v2 tag 的取值，`:569-613` 的 `qup_i2c_set_tags()` 给出打包顺序，
`:152` 的 `READ_RX_TAGS_LEN 2` 给出核心回吐几枚 tag 字节。逐条对 `InputRegs.h:269-274` 与本驱动
`chipRead()`：

| 事实 | 主干 | 本仓 | 判定 |
| --- | --- | --- | --- |
| START tag | `QUP_TAG_V2_START 0x81`（`:107`） | `kQupTagV2Start 0x81U` | 同 |
| 地址字节 | `i2c_8bit_addr_from_msg()`，即 `(addr<<1) \| rd` | `address << 1` / `(address << 1) \| 1` | 同 |
| DATAWR / DATAWR_STOP / DATARD_STOP | `0x82 / 0x83 / 0x87`（`:108-112`） | 同值 | 同 |
| 写段的 STOP 变体 | 只在 `last` 时用（`:591-603`），两段式事务的写段用 `0x82` | `wire[2] = outLen ? DATAWR : DATAWR_STOP`，两段式恒取 `0x82` | 同，且 STOP 那一支在本驱动里**永不触发**（`outLen` 恒非零，见 §6） |
| 读段写进 OUT FIFO 的 tag 字节数 | 4（`qup_i2c_set_tags` 给读消息也是 START+addr+DATARD_STOP+len） | `Segment read { tags, kQupMaxTagsLen=4, … }` | 同 |
| 回吐进 IN FIFO 的 tag 字节数 | 2（`:1479 blk->rx_tag_len = 2`） | `kQupRxTagLen 2U` | 同 |
| `MX_WRITE_CNT` / `MX_READ_CNT` 填什么 | `qup_i2c_conf_count_v2()`：写= `total_tx_len`；读= `rx_tag_len + data_len`（`:1478-1483`） | 写段 `seg.txLen`；读段 `kQupRxTagLen + seg.rxLen` | 同 |
| 长度字段里 0 的含义 | `QUP_READ_LIMIT 256` 编码成 0（`:607-609`） | `chipRead()` 直接拒绝 `outLen > kQupReadLimit - kQupRxTagLen` | 同，且拒得更早 |

**结论：四笔子传输全部 NACK，不是因为我们的 tag 写错了。** 这是触摸线到目前为止最值钱的一句排除——
它以前只是没被怀疑过，现在是**对着源码逐字节比过之后再排除**。

## 3. `I2C_STATUS` 的位表，以及十四靴以来一直看不见的那两枚

`i2c-msm-v2.h:121-136` 是本板厂商树自己给出的命名（主干只命名了其中四枚）：

| 位 | 名（厂商头） | 主干是否命名 |
| --- | --- | --- |
| BIT(2) | `QUP_BUS_ERROR` | 否 |
| BIT(3) | `QUP_PACKET_NACKED` | 是（`QUP_I2C_NACK_FLAG`） |
| BIT(4) | `QUP_ARB_LOST` | 否 |
| BIT(5) | `QUP_INVALID_WRITE` | 否 |
| BIT(6) | `QUP_FAILED` | 否 |
| **BIT(7)** | **无名** | 否 |
| BIT(8) | `QUP_BUS_ACTIVE` | 是 |
| BIT(9) | `QUP_BUS_MASTER` | 是 |
| BIT(23) | `QUP_INVALID_TAG` | 否 |
| BIT(24) | `QUP_INVALID_READ_ADDR` | 否 |
| BIT(25) | `QUP_INVALID_READ_SEQ` | 否 |
| BIT(26) | `QUP_I2C_SDA` —— 核心自己看到的 SDA 线电平 | 否 |
| BIT(27) | `QUP_I2C_SCL` —— 同上，SCL | 否 |

两棵树的**错误掩码本身就不一致**：主干 `I2C_STATUS_ERROR_MASK = 0x38000FC`（含 BIT(6)、BIT(7)），
厂商 `QUP_MSTR_STTS_ERR_MASK = 0x380003C`（不含）。本驱动用的是主干那枚（`kI2cStatusErrorMask`）。
这不是一句注释就能收的分歧，因为它会改变判据：一枚只置了 BIT(6)/BIT(7) 的状态字，
按主干算失败、按厂商算什么都没发生。所以 r318 的行把**未遮罩的原字**和两种掩码的结果一起带上。

**本轮量出的仪器事故**：`clover_input_observation[6]` 存的从来是
`qupRead(kQupI2cStatus) & kI2cStatusErrorMask`，而掩码 0x38000FC 的最高位是 BIT(25)——
**BIT(26) SDA 与 BIT(27) SCL 被这枚掩码永久抹掉**。boot 48 那行
`status=10000c8`（r260 beat，`notes/development-log.md` 的 r260 读数一节）里的 SDA/SCL
两格恒为 0，不是「线没被拉低」，是**这一格结构上不可能有读数**。
而 `CloverInput.cpp` 结尾那句「sda=%08x scl=%08x」读的是 **TLMM 的 IO 寄存器**——
 gpio10/11 此刻的 mux 是 `blsp_i2c3`，那两格的输入状态位归 QUP 驱动，不是 TLMM。
两个位置都不对，所以「有人在传输中途把线拉低」这条 r181 源码自己写下的替代解释，
**从 round181 到 round317 一次也没被仪器问过**。r318 修的就是这一格。

## 4. 厂商对触摸地址 NACK 的判读，以及它够不到本板的那一格

`i2c-msm-v2.c:92`（诊断打印，函数 `i2c_msm_dbg_dump_diag`）：

```c
if ((status & QUP_PACKET_NACKED) && (xfer->msgs->addr == 0x38 || xfer->msgs->addr == 0x1d)) {
	dev_dbg(ctrl->dev, "%s: i2c slave suspended", __func__);
	return;
}
```

同一文件 `:70` 的错误表把 NACK 直译成 `"NACK: slave not responding, ensure its powered"`。
两句合起来是本板厂商树自己的判读：**对触摸地址的 NACK 是「从机在睡」，第一步是查电，不是判器件不在**。
注意它点名的是 `0x38` 与 `0x1d`——**本板 DT 挂的是 `focaltech@38` 与 `gt9xx@5d`**，
厂商那条特判覆盖 `0x38` 而不覆盖 `0x5d`；`0x1d` 是 Goodix 另一种地址拨法，本板不是。
所以借来的是**判读顺序**（先电、后总线、再协议），不是那段代码。

`:88` 附近同一函数的 timeout 分支还有一句更可用的：不是 bus master 或 SDA/SCL 为低时，
厂商写的是「slave 把线拉住了，或者 GPIO 配错」——这正是 §3 那两枚我们第一次要读的位。

## 5. FIFO 尺寸：卡上的 `io=c0a5` 解出来是 64/64，因此 OUT_SERVICE 那一格是惰性的

`InputRegs.h:262-265` 的四个字段来自 `i2c-qup-mainline.c:1856-1890`，块尺寸表 `blk_sizes[] = {4,16,32}`
在 `:1687`。boot 48 的 `io=c0a5`：

| 字段 | 取位 | 值 | 尺寸 |
| --- | --- | --- | --- |
| out block | `bits[1:0]` | 1 | 16 |
| out fifo | `bits[4:2]` | 1 | `16 * (2 << 1)` = **64** |
| in block | `bits[6:5]` | 1 | 16 |
| in fifo | `bits[9:7]` | 1 | `16 * (2 << 1)` = **64** |

（`0xc000` 是 PACK\|UNPACK，即本驱动 `bringUpCore()` 写进去的 `kQupRepackEn` 读回来了。）

于是两处形状差异都变成可判定的：本驱动每次事务最多 7 字节（4 tag + 2 寄存器地址 + 1 数据），
远小于 64，所以 `pushTx()` 一趟就写得完，OUT_SERVICE 那格的「把 room 重置成整条 FIFO」与主干
「FIFO 模式下 OUT_SVC 且无 `OUT_BLOCK_WRITE_REQ` 时什么都不做」（`i2c-qup-mainline.c:338-349`）
**永远不会分岔**；主干 mode 选择本身也是按「≤ out_fifo_sz 走 FIFO」挑的（`:1552-1563`），
所以对这一切请求，两棵树选同一条路。宿主模型按 64/64 与 4/4 两档都断言了这条不等式成立。

收方向的分歧是同源的但相反：主干在 IN_SERVICE 时按整条 FIFO 加 `fifo_available`，
本驱动逐字核对 `QUP_IN_NOT_EMPTY`（BIT5）才读一枚字——这是 round98 在 eMMC FIFO 上学的规矩，
它只会读得比硬件给的少，不会多。模型里两条路对同一串字节给出同样的数据。

## 6. Goodix 的身份解码与复位序列，逐条对过厂商

`gt9xx.c:1222-1258`（`gtp_get_fw_info`）的缓冲是 `u8 buf[8] = {GTP_REG_VERSION >> 8, GTP_REG_VERSION & 0xff}`，
即 **前 2 字节是自己写进去的寄存器地址，回话落在 buf[2..7]**。本驱动的 `first[6]` 正是那 6 枚数据，
所以厂商的 `buf[2]/[3]/[4]` = 我们的 `[0]/[1]/[2]`，`buf[5] != 0 → pid 取 4 枚` = 我们的 `[3]`，
`(buf[7] << 8) | buf[6]` = 我们的 `(first[5] << 8) | first[4]`——`InputRegs.h:322-326` 那段索引说明成立，
且 `GTP_REG_VERSION 0x8140`、`GTP_REG_SENSOR_ID 0x814A`、`GTP_ADDR_LENGTH 2`、
`sensor_id >= 0x06 视为无效`（`:1250`）与 `gt9xx.h:71,359,363,364` 一致。

一处**本轮改正**：厂商真正做「读两遍并要求一致」的是 `gtp_i2c_read_dbl_check()`（`gt9xx.c:217-246`），
它被用在 **sensor_id** 上；`gtp_get_fw_info()` 读版本号只读一遍。本驱动反过来——
版本读两遍、sensor_id 读一遍。两条都是「同一问自答两次」，不是缺陷，但档案里
「because a bus nobody has driven before can produce a plausible first answer」那句挂在版本上的
理由，应当记成**我们的**选择而不是厂商的做法。厂商 dbl_check 还有一格我们没抄：它先把缓冲填
`0xAA`/`0xAB` 再比，所以「没读到」和「读到 0」在厂商那里分得开，在我们这里两遍都是零填充。

`gt9xx.c:532-561`（`gtp_reset_guitar`）的时序：reset 拉低 → T2 = 参数（本驱动 20 ms）→
`gtp_int_output(ts, client->addr == 0x14)` → T3 2 ms → reset 拉高 → T4 6 ms →
**再拉高一次** → `gtp_int_sync(ts, 50)`。那次重复的 `gpio_direction_output(rst, 1)` 是厂商原文，
不是本驱动的笔误；模型照这个形状断言，防止后续轮次把它「清理」掉。

## 7. 现在能说什么，不能说什么

已证：控制器驱动得起来（`io`/`clk`/`state` 都是活值，boot 48 四拍同形）；
两个候选地址的四笔**写段**全部 NACK、零次超时；tag 线码逐字节对得上主干（§2）。

未证，且本轮第一次变成可问的：**面板是否把 SDA/SCL 压着**（§3，BIT26/27 从未被读进任何字段）；
**触摸的两颗 supply `pm660l_l3` / `pm660_l11` 到底有没有 enable**——那两枚 enable 至今一次没读到
（r182 停在 `not a v5 arbiter (03000000)`，r313 把地板图改成按版本字选表，等 boot 104 落回）；
以及「NACK 是地址级还是数据级」——本板四笔失败全发生在写段的第一个字节上，这一点由
`xfers=4 nack=4` 与 `chipRead()` 的返回顺序可推，但厂商位表里没有一枚位专门命名「地址 NACK」。

排期 4 的下一步因此不由本区决定：电那一格属 CloverPower（r313 已上膛），
本区能自己推进的是把 §3 那两枚线电平读进卡上文本，并把 `status` 从「遮罩后的十六进制」
变成「有名字的十个条件 + 一枚无名残数」。
