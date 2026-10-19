# ttp_asf

原版 TTPlayer ASF Reader、DMO Decoder 两个工厂及接口的独立源码实现。
使用系统 Windows Media Format Runtime 读取 WMA ASF；MP3-in-ASF 在系统读取器拒绝时由自有包解析接管，再由系统 DMO 解码；
不依赖旧 ttp_asf.dll，也不把微软闭源编解码内核当作本项目重新实现的代码。

```powershell
./build.ps1 -Package -PackageVersion 2026.10.19
```

需要现代 MSVC x86、Windows SDK、CMake、Python、PowerShell。兼容层固定为
VC-LTL 5.3.1、YY-Thunks 1.2.2；源码在构建时下载校验，许可证保存在仓库。
Action 仅手动构建，Release a Version 默认关闭；不运行或上传私有测试。
运行 ZIP 仅含 `AddIn/ttp_asf.dll` 与 `SHA256SUMS.txt`，版本使用北京时间日期。

## 实现

- Reader 保留压缩音频输出，Decoder 单独接收压缩包，兼容旧宿主的两个阶段。
- 从系统目录加载 wmvcore.dll、wmadmod.dll；原版的通用 DMO 枚举分支也保留。
- 按原代码设置 `_HIRESOUTPUT`，协商首个合法输出格式；识别宿主给出的
  WAVEFORMATEXTENSIBLE 容量，保留 24-bit 和 5.1 输出，不固定截断为 16-bit。
- IStream 桥接宿主短读及扩展取消接口；保护小缓冲区，保留不足容量的完整压缩包。
- MP3-in-ASF 自有解析保留多 payload、压缩 payload、跨包对象重组；预扫实际对象容量并建立安全定位起点，避免大对象反复返回容量不足。
- 标签支持 Unicode、Date/Year、Tracknumber/Track 等宿主字段映射；封面支持
  读取、添加、删除及按 mask 修改。保持原接口的 1 张 / 60000 字节写入限制。
- 修改先保存到同目录临时副本，系统编辑器完成后再替换原文件；新增可选 Commit
  返回真实结果并锁存重复调用。旧宿主未调用 Commit 时在最终 Release 保存。
  只读文件、网络流和可识别的虚拟/包内流拒绝修改。

## 边界

XP 必须具有支持 WMCreateSyncReader 的 Windows Media Format Runtime；此 DLL 不分发
微软运行时。系统未装对应 DMO、受保护内容或特定编码不被系统支持时返回失败。
当前 XP VM 缺少该 MP3 解码 DMO，原、新独立 ASF Decoder 都失败；但原播放器能通过内置 MPEGDecoder 接收新版 Reader 的压缩输出，已实测播放成功。重建宿主也需要对应的内置 MPEG 桥接，不能把缺少 DMO 等同于播放器不支持 MP3-in-ASF。
WMA Reader 使用系统读取器；MP3-in-ASF 自有解析目前要求可定位的完整输入，首次打开需顺序预扫全部数据包（不保留负载），最大对象64 MiB。网络流、损坏文件、语言属性、多音轨选择的
所有边界尚未完成逐项原版对照，不能称为完全等价。

只有 18 字节输出格式容量的宿主可能协商为 16-bit，5.1 可能被拒绝；宿主应将
40 字节缓冲区的 `cbSize` 设置为 22，才能明确允许完整高精度格式。
Win11 的 24-bit Lossless / Pro 立体声与 5.1 已实测；其它系统结果见验证记录。

[验证记录](docs/VALIDATION_20261019.md)；包装层源码许可证 [MIT](LICENSE)。
