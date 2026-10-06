# 来源与许可

- BBKH1-MiniGPT 原创推理、MXU/DMA 适配、界面、测试和构建代码随 GPL-2.0 发布，见 LICENSE。维护者：HelloClyde。
- `src/libc/freestanding.c` 和初始 freestanding 声明改编自 `HelloClyde/BBK9588-gba` 的 `57c2792ec374b394df24c0ee898bb8624cf95a85`，上游 GPL-2.0。H1 的内存、文件和时间接口替代了原固件依赖。
- `sdk/` 是 `MrDefinition1999/bbk-h1-bda-sdk` 的 `067fe072477861dfc8949d7b1a55279fb92d2548`，独立采用 Apache-2.0；保留 SDK LICENSE 和 NOTICE。
- 模型权重和 `token_data.h` 源自 `jingyaogong/MiniMind2-Small`，固定版本 `8c0c0de640cd532fee03d329b95a58c56591b5bc`，Apache-2.0。原始 safetensors SHA256 为 `83bfe6f127c98120a3410aab65eee3b66b8301ac352c20b7efd5e2eb688f6d85`。导出脚本在本地读取张量并量化，没有执行上游 Python 模型。
- `font_data.h` 为 GNU Unifont 16.0.04 的位图子集，子集名称 MiniGPT UI Bitmap，选择 SIL Open Font License 1.1。完整版权/许可说明见 `LICENSES/Unifont-COPYING.txt` 和 `Unifont-README.txt`。GBK 表由 Python 标准字符映射生成。
- `experiments/minigpt/icon.png` 为项目聊天气泡/点阵 AI 图标，与应用源代码一起发布；不含第三方应用图标像素。
- 工具链 URL 与 SHA256 参考 `HelloClyde/bbk9588-bda-sdk` 的 `870470ce09a5b33ae9b2d0c1b8e40c0435751a98` 配置；安装脚本是本项目实现。下载的 GCC/libgcc 保留压缩包自带许可，不进入源码仓库。
- `docs/screenshots/` 来自真实运行的项目 BDA。系统输入法等固件界面、名称及商标仍归各权利人所有，截图仅作行为验证记录，不授予其中第三方内容的 GPL 许可。
- 主机 RAM fixture 是由零字节和 CRC 后缀生成的合成测试数据，不是固件代码。模型文件在 Release 交付，原机固件/NAND/系统应用/输入法词库不交付。
