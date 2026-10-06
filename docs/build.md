# 构建与测试

README 的 Windows / Python 3.11 命令是发布构建入口。工具链下载包 SHA256：`8ba866e25c9826ee04ab4310365d264e3e73769e3738bb58ae38fd6740b7ee8d`，编译器 GCC 15.2.0。已有工具链使用 `--toolchain` 或 `H1_GNU_BIN` 指定；主机 GCC 通过 `HOST_GCC` 或 PATH 查找，运行时 DLL 路径由编译器目录加入。

## 主机验证

先构建两版 BDA、执行 `python tools/export_release_models.py`。安装本机 MinGW/MSYS2 GCC，设置 `HOST_GCC` 后运行：

```powershell
python tools/prepare_test_fixture.py
python tools/test_minigpt.py
python tools/test_minigpt_int4.py
python tools/test_minigpt_prefill_cache.py --int8
python tools/test_minigpt_prefill_cache.py
python tools/test_minigpt_layer_cache.py
python tools/test_minigpt_dma_auto_host.py --mxu
python tools/test_minigpt_dma_auto_host.py --mxu --int4
python tools/test_minigpt_mxu.py
python tools/test_minigpt_int4_kernel.py
```

测试输出仅写入 `build/`。原始 F16 权重的 NumPy 对照、量化格式、分词、连续/流式推理、前缀 KV、取消/边界/分配失败、缓存策略、DMA 挂钩状态机均有检查。RAM fixture 完全合成；它验证挂钩控制流程与错误处理，不证明某份真实固件与签名一致。Unicorn 运行普通 MIPS，MXU 扩展用明确的指令解码器解释，验证结果与寄存器保护，不测真实芯片周期。

## 生成资源

正常构建使用已提交的 `font_data.h`、`token_data.h` 和 `gbk_data.h`。如需从固定上游再生，执行 `python tools/export_minigpt.py`；该步骤会重写生成头文件、导出 Q8，并下载 Unifont 16.0.04 的字体和许可说明。使用 Python 3.11 保持 Unicode 范围表一致，再生后需重新核对 BDA 散列。仅构建安装包应使用 `export_release_models.py`，它不会重写源码头文件。

## 发布

`main` / PR / 手动触发运行构建与主机测试。`v*` 标签在相同构建成功后创建 Release，使用该次 artifact 中的 BDA、两份安装 ZIP 与 SHA256SUMS。仓库发行标签以默认 INT8 应用版本为基准，可带构建流程修订号，例如 v0.3.6+build.1；同次包内的 INT4 有独立版本 0.4.2。构建修订不改变设备程序版本。

```powershell
python tools/package_release.py
```

安装包只收录指定 BDA、模型、许可、README、验证记录和对应源码。源码包包括固定 SDK 必需部分及 `PINNED_REVISION`，无需下载模型即可再次重建 BDA。检查 tag 格式与 VERSION 一致，已有 Release 不自动覆盖。
