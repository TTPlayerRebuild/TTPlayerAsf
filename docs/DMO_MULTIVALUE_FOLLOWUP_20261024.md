# 系统 DMO 与同名多值复核（2026-10-24）

## 结论

本轮在 Windows 11 上复核当前系统提供的 WMA/MP3 DMO，并补充输出容量、未读 PCM 的 Reset，以及同语言重复属性的实际保存行为。未发现需要修改生产代码的失败；也没有注册、安装或替换系统/第三方 DMO。

## DMO 解码与边界

| 本地样本 | 原/新 PCM 字节数 | 输出 |
| --- | ---: | --- |
| WMA2 | 352256 | 16-bit stereo / 44.1 kHz |
| WMA Pro | 288000 | 24-bit stereo / 48 kHz |
| WMA Lossless | 864000 | 24-bit 5.1 / 48 kHz |
| MP3-in-ASF | 357312 | 16-bit stereo / 44.1 kHz |

四组原 DLL 与当前 DLL 的全量 PCM 逐字节一致。Lossless 输出另与编码前的源 PCM 完全一致。共 8 次原/新全量解码调用、4 次边界调用及 4 组摘要比较通过。

每个样本对新 Decoder 还验证：

- 容量为 `blockAlign - 1` 时返回 `E_INVALIDARG`，长度清零；之后用正常 Buffer 重试不会丢失该包 PCM。
- 容量为 `2 * blockAlign + 1` 时按完整采样帧分段返回；全曲结果与 65536 字节容量完全一致。
- EOF 后 Reader 退回 0、Decoder Reset，再次全量解码一致。
- 只取一个采样帧、留下尚未消费的内部 PCM 后 Reset，Reader 退回 0，输出与新播放完全一致，没有混入 Reset 前剩余样本。

这里的 MP3 通过本机已注册的音频 DMO 回退路径；不推定 XP 具有同一 DMO。此前 XP 缺少对应 MP3 DMO、宿主内置 MPEG 路径仍可播放的区别保持不变，见 [2026-10-19 验证](VALIDATION_20261019.md)。DRM 和第三方 DMO 不在本轮通过范围。

## 同语言重复属性

通过系统 `IWMHeaderInfo3::AddAttribute` 建立同名自定义属性：默认语言有 `First value`、`Second value` 两项，日语有 `Japanese value` 一项。每次操作使用独立文件副本，保存后重新通过系统元数据接口读取实际磁盘结果。

当前 DLL 的行为：

| 操作 | 磁盘结果 |
| --- | --- |
| Count/At 枚举 | 保留三项；日语名称为 `X-Rebuild-Multi[lang=1]` |
| 无后缀 Get | 返回默认语言的第一个值 `First value` |
| 只修改 Title | 三项原值全部保留 |
| Set 同名默认语言字段 | 默认语言两项合并为一个新值；日语保持不变 |
| 删除默认语言字段 | 默认语言两项均删除；日语保持不变 |

原 DLL 在本轮同一文件的枚举/Get 对照中只暴露第一项。这不意味着磁盘上原来的其他值不存在。重建版的 Count/At 已能够完整列出上述重复项，但旧 `Set(name, value)` ABI 没有“第几项”的参数，因此当前 Set 的语义仍是**替换所选语言的整个同名组**。不能将其称为逐项多值编辑器。此限制不影响修改其他字段时保留原重复项。

## 证据

本轮重建 DLL SHA-256：`bd905d03f7b2a0601bcd0484aea64ca485839f25b4700b101cf333306b221b5d`，与本轮开始时相同。原 DLL SHA-256 和原版 DMO 调用链继续使用 [既有分析](VALIDATION_20261019.md#原版证据)。

私有脚本、两个新增探针、来源样本及结果位于工作区 `rebuild/tests/plugins_followup_20261024/legacy`，其中 `asf-dmo-results.json`、`asf-metadata-results.json` 保存本轮输出。测试不上传、不进入 Action、不放发行包。本轮没有重跑虚拟机或播放器界面，不把系统 DMO 接口成功扩大为所有音轨、所有系统、所有故障输入均通过。
