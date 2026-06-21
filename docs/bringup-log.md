# P0 硬件 Bring-up 日志

板：Waveshare ESP32-S3-Touch-LCD-1.85C V2 · 工具链：ESP-IDF **v5.5.4**（`source ~/.espressif/tools/activate_idf_v5.5.4.sh`）· 串口 `/dev/cu.usbmodem1101`

## 关键决定 / 踩坑

- **IDF 版本**：v6.0.1 编不过音频/唤醒栈（esp-sr 锁 esp-dsp 1.6.0、gmf、内置 json 组件被移除），降到 **v5.5.4** 全部开箱即用。
- **板子健康**：官方 demo 上板亮出彩条（demo 自身因内存 alloc 崩溃 boot-loop，与我们固件无关）。
- **复位脚在 TCA9554**：屏 RST=EXIO2、触摸 RST=EXIO1（非直连 GPIO）。
- **显示**：寄存器 0x04 = `00 02 7f 7f` → 用 `vendor_specific_init_new`（case 2）。
- **esp_codec_dev 地址**：要 8-bit 形式（ES8311 0x18→**0x30**，ES7210 0x40→**0x80**），内部右移。
- **esp-sr 2.4.6**：无 `USE_AFE/USE_WAKENET/MODEL_IN_SPIFFS` 符号（AFE/WakeNet 恒编入）；唤醒词符号是 `CONFIG_SR_WN_WN9S_NIHAOXIAOZHI`；`esp_afe_handle_from_config` 在 `esp_afe_sr_models.h`。
- **sdkconfig.defaults 只在无 sdkconfig 时生效**——改完要 `rm sdkconfig` 重生成。
- **喇叭功放使能 = GPIO15 拉高**（NS4150B）。bsp_board 的 `GPIO_PWR_CTRL=-1` + PA 初始化注释掉，对本板是错的；以 Waveshare Arduino 例子 `digitalWrite(15,HIGH)` 为准。设 `es8311_codec_cfg.pa_pin = 15`。**调试教训**：mic 好但喇叭哑 → 输出模拟链；不是格式/MCLK，是功放没使能。麦克风走 ES7210 不经功放，所以一直正常。

## 完成情况（软件层已自验证；物理现象待统一测试）

| 任务 | 状态 | 串口自验证 |
|---|---|---|
| T2 脚手架 | ✅ | PSRAM 8.0MB、内部 RAM 375KB |
| T3 I2C+TCA9554 | ✅ | 扫到 0x15/0x18/0x20/0x40/0x51 |
| T4 ST77916 屏+背光 | ✅ | case2 init、color bars |
| T5 LVGL | ✅ | LVGL ready、label shown |
| T6 CST816 触摸 | ✅ | IC id 181、注册 indev |
| T7 ES8311 出声 | ✅ | output ready、tone done |
| T8 ES7210 录音回环 | ✅ | input ready、loopback done |
| T9 esp-sr 唤醒(你好小智) | ✅ | model 加载、feed chunk512/nch1、started |
| T12 电池 ADC | ✅ | cali OK、4.22V |
| T10 自定义词 你好小葡萄 | ⏸ 另起 | 见下 |
| 4ch RMNM 升级 | ✅ | ES7210 全4麦 TDM, AFE "RMNM"(ch0=AEC参考), feed nch=4; 唤醒+echo 上板验证(听到回声) |
| T11 AEC barge-in | 🟡 基础就位 | AEC 参考通道(RMNM ch0)已馈送; 完整 barge-in 待 P3 音乐(持续音源)验证 |

## 统一硬件测试清单（请上板逐项确认）

烧录最新固件后复位，观察：
1. **屏**：圆屏稳定显示「XiaoPuTao P0」+「TAP: 0」按钮（不闪烁）。
2. **触摸**：点按钮 → 计数 +1（串口同步打 `touch tap #N`）。
3. **出声**：开机听到两声 1kHz 提示音（`tone done`）。
4. **录放**：提示「recording 3s」时说话 → 3 秒后听到自己的回放。
5. **唤醒**：说「**你好小智**」→ 串口 `WAKE DETECTED` + 一声短 beep。
6. **电池**：接锂电后串口电压接近万用表实测（USB 下读充电轨 ~4.2V）。

监看串口：`idf.py -C <repo> -p /dev/cu.usbmodem1101 monitor`

## T10 自定义唤醒词「你好小葡萄」—— 另起工作流

esp-sr 2.4.6 **不含** DIY 的 WakeNet 自定义训练工具（`tool/` 只有 MultiNet 的 g2p/pinyin）。自定义 WakeNet 词需要：乐鑫官方定制训练服务，或外部 TTS 样本训练流水线。**当前唤醒链路已用内置「你好小智」完整跑通**；拿到训练好的「你好小葡萄」模型后，替换 `CONFIG_SR_WN_WN9*` 选择 + 重刷 model 分区即可，固件无需改动。→ 作为独立子任务推进。

## T11 AEC barge-in（音乐中打断）—— 待硬件在环调

当前 AFE 用单麦「M」格式（无 AEC）。barge-in 需要 AFE 拿到喇叭回采参考通道（格式改 "MR"/"MMR"），有两条路：① ES7210 硬件回采通道（需确认是否布线）② 软件把播放 PCM 作参考喂入。两者都**必须边放音乐边念唤醒词、靠耳朵和误唤醒率调参**，因此放到统一硬件测试时在环完成。这是 P0 设计里标注的最高风险点。
