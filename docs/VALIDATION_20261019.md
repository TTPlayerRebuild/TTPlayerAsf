# ASF 恢复与验证，2026-10-19

## 原版证据

基准 `AddIn/ttp_asf.dll` SHA-256：
`f36dcc9104e7bd11100ec983c03b0183da5a9d77592b806fe8a909d87d18fcf5`。
REA 6.5.0 / Ghidra 12.1.3；证据与私有测试均在
`rebuild/tests/audio_plugins_fix_20261019/asf`，没有覆盖原文件或修改用户音乐。

- `60155CAF` 加载 wmadmod、DllGetClassObject，系统 WMA 是外部内核。
- `601556F7` 返回 S_FALSE，作为通用 DMO 候选；不是仅声明 WMA GUID。
- `601557A9` 先直接加载 WMA DMO，失败后枚举注册的音频 DMO。
- `60156567` 设置 IPropertyBag `_HIRESOUTPUT=true`，读取首个合法媒体类型；
  检查宿主 `18 + out->cbSize` 容量，后续尝试浮点、输入位深和 16-bit 回退。
  Evidence：`ev_5291e02b900ba5a0204095c70c2c00efa0b6080d5971ea7cc7213e4242df91d3`
  对应 Initialize；完整配置函数另存 `types.json`。

## Win11 对照

- WMA1 / WMA2 × mono / stereo × 32 / 44.1 / 48 kHz：12 个样本。
  每个测试旧 Reader+旧 Decoder、旧+新、新+旧、新+新，共 **48 次成功**，各组 PCM 完全相同。
- 系统 IWMWriter 从有低位随机信息的 24-bit PCM 生成 Lossless / Pro × stereo / 5.1
  四个样本；每个测试 basic、extended、float 请求与上述四种交叉，共 **48 次**。
  32 次成功的 PCM 与原插件逐字节一致；16 次六声道 basic/float 请求被旧、新插件均拒绝，
  HRESULT 不要求相同。extended 全部输出 24-bit，两个 Lossless 文件与源 PCM 完全相同。
- 只有 `cbSize=0` 的立体声请求，原、新都会退为 16-bit；`cbSize=22` 输出完整
  WAVEFORMATEXTENSIBLE（24-bit / 正确声道）。这不是通过保留 16-bit 固定策略实现的对照。
- 标签新增、删除、Unicode、Date/Tracknumber 别名、未知字段保留通过；原插件能读回新写入的
  Title、Artist、Date、Tracknumber。封面描述 mask 保留图像/MIME/type，保存后重读一致。
- WM_PICTURE 是 packed(1)，字符串可在奇数偏移，已修复读入时的对齐假设。
- 7 字节短读、1 字节和零容量缓冲区、Stop/Start、seek 后首包一致、网络及只读拒写通过。
- 保存前权限改变导致真实写失败，Commit 返回 `C00D0018`，重复结果一致，原文件标题不变；
  失败临时副本清理包括只读属性。

## 系统与边界

首轮 WMA 验证 DLL SHA-256 `b6cb65bc9c193fec5fd6022b938e23ae608acce90845676d2de43530526cfb0a`，81920 字节；后续 MP3 分支修复后的最终版本另列于下文。
XP / Win7 各执行 VM stage 的六项测试全部通过：WMA2、Pro stereo/5.1、Lossless stereo/5.1、
标签/封面/容量/定位/取消/保存失败。高精度输出分别为 288000 / 864000 字节，
保存失败均为 `C00D0018`。日志保存在私有 `asf/xp-run.log`、`asf/win7-run.log`。
这些原生接口测试不能代替全部宿主 UI 测试。
暂未覆盖 DRM、所有注册第三方 DMO、实时网络取消、复杂损坏 ASF、多语言属性、多音轨选择。
文本枚举现在保留一个非空同名值；不能据此宣称完整恢复多值/多语言编辑器。

本项目不引用可自行升级的微软解码源码库；使用目标系统提供的 WMF / DMO。
官方 API 依据：[Windows Media Audio Decoder](https://learn.microsoft.com/en-us/windows/win32/medfound/windowsmediaaudiodecoder)、
[枚举编码格式](https://learn.microsoft.com/en-us/windows/win32/wmformat/to-enumerate-codec-formats)、
[WM/Picture](https://learn.microsoft.com/en-us/windows/win32/wmformat/wmpicture)。

## 原生 ASF 包与宿主状态机补齐

原 `60157791 → 60158012` 自行读取 ASF packet，`60157C2F` 解析变长头，
支持多 payload、replicated length=1 的压缩 payload、跨包 media object 重组。
`601585B2` 要求调用方容量能够容纳完整对象；不足时原版返回0，外层误认为结束。
新实现保留完整对象，拒绝不足容量并可重试；预扫最大对象长度后对宿主声明真实 BufferSize。
预扫顺序读取全部数据包，只解析头、不复制/保存负载；这是系统读取器拒绝的 MP3-in-ASF
分支的首次打开成本，正常 WMA 不额外预扫。

`601578A1 → 60158311` 证实原版按数据包粒度定位，而不是按每一个MP3帧定位。
新版记录安全对象起点，正常单/多/压缩payload的六个定位目标与原版首包一致。
原版在跨包对象中间定位会丢掉该对象，新版回到对象首包，避免跳歌或错误EOF；
这项是修正原边界，不标记为逐字节复刻原有错误。

Win11 私有验证：

- MP3-in-ASF 旧/新 Reader 与 Decoder 组合，PCM 为357312字节，内容一致。
- 六种包布局（原样、单、多、分片、压缩、压缩多payload）24次Reader调用的数据/对象边界一致；
  此组使用32kbps样本验证包解析，未将其当作系统DMO解码通过。
- 六种布局各六个定位目标、旧/新共72次检查；分片情况记录上述有意修正，其他首包一致。
- 97800字节对象跨约1900包，BufferSize正确；0/500/1000ms定位保留完整对象，截断输入拒绝。
- MP3分支标签/封面/掩码/原子保存与7字节短读整套通过，原版可读回保存后的通用字段。
- WMA常规48次、高精度48次交叉测试在最终解析修改后全部复跑。
- 原 `601557A9` 调用 AllocateStreamingResources；新Decoder已恢复。
  DMO暂时没有PCM不能作为EOF：新版返回S_OK/0字节，继续请求输入。
  实际PluginManager的24bit Lossless首包无PCM路径已验证，输出288000字节。

官方容器依据：[ASF规范入口](https://go.microsoft.com/fwlink/?LinkId=89814)，版本1.20.03第3–5节。
自有分支仍限定MP3、完整可定位输入、无DRM、无需audio-spread重排；不宣称任意损坏或实时ASF等价。

### 最终产物与失败后恢复

最终发行 DLL 为92672字节，SHA-256：
`9a962df7d7d6c1f8197f881178c3ec7ca9240c9047044265bd94242b5b267218`。
VM stage 与运行ZIP的DLL一致。
长对象定位预扫故意在第200次7字节读取后注入STG_E_READFAULT，
再次Start/Read不会沿用未保留数据的分片；重新定位0后97800字节完整一致。
此测试是原生故障注入，不依赖屏幕操作。

当前XP VM未安装可解码该MP3格式的系统DMO：
原ASF Decoder返回80004002，新Decoder返回80004001，均未成功；
Reader包解析、WMA和高精度路径测试通过。独立DMO的结果须与宿主内置MPEG路径区分。
已实测：XP 原播放器加载最终新 ASF DLL 可以播放 MP3-in-ASF，PlayingTime=2740ms。
因此，原/新独立 ASF Decoder 的失败只说明该系统没有对应 DMO，不能据此判断整个播放器无能为力。
原 EXE 内置 MPEGDecoder 通过 ttpcomm ordinal 11/12/13 接收 Reader 输出；
重建宿主此前缺少这个内置 MPEG 承接分支，现已按原版恢复，ASF DLL 保持上述最终哈希。

最终宿主验证：

| 环境 | 原版 WMA / MP3 播放时间 | 重建版 WMA / MP3 播放时间 |
| --- | --- | --- |
| Windows 11 | 3801 / 3831 ms | 3740 / 3810 ms |
| XP SP3 | 2410 / 3330 ms | 3110 / 2840 ms |
| Win7 SP1 | 3810 / 3820 ms | 3790 / 3790 ms |

均核对实际加载的最终插件路径、无错误对话框、正常关闭；时间为自动检查时保存的播放进度，不作为音频总时长或性能比较。
同一最终宿主的19项全流解码集成在三个系统全部通过；12秒MP3-in-ASF经内置MPEG输出8497152字节float64，461帧全部保留。
因此XP系统缺MP3 DMO的独立接口失败不再阻止重建播放器播放此编码。

另以宿主实际使用的 `STGM_SHARE_DENY_WRITE` 方式打开 MP3-in-ASF，标签/封面/原子保存全套仍通过。
