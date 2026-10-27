# shm_web —— ROCOS 共享内存实时观察与调试工具（Web）

供 **rocos-ecm 和 rocos-igh 共用**的工具：**C++17 后端**（读懂 POSIX 共享内存）+ **纯 HTML/JS 前端**（浏览器实时展示）。
直接使用公共库 [rocos_shm/shared_memory_config.hpp](../../include/rocos_shm/shared_memory_config.hpp) 的客户端路径连接到主站进程创建的 ABI v2 共享内存，
把总线快照与 PDO 变量值以 JSON 暴露，并用 Server-Sent Events 周期推送，实现"动态显示"。

## 目录

- `shm_web_server.cpp` —— HTTP 后端：静态文件 + `/api/snapshot`（JSON 快照）+ `/api/events`（SSE 推送）。
- `index.html` —— 前端单页：总线状态卡片、周期统计、每个从站的输入/输出 PDO 变量实时值。
- `controls.mjs` —— 浏览器写权限、主站状态请求、SDO 读写与命令结果。
- `diagnostics.mjs` —— ABI 字段、有效位和诊断数据展示。
- `value_decoder.mjs` —— 小端 PDO 数据类型解析与写入编码，供浏览器页面与 Node.js 测试复用。
- `CMakeLists.txt` —— 构建脚本（独立于主工程，可单独编译）。

## 构建

以下命令在 `rocos-shm` 仓库根目录执行，需要 CMake 3.16+ 和 C++17 编译器；不依赖 ECM/IgH SDK 或主站仓库。

```bash
cmake -S tools/shm_web -B tools/shm_web/build
cmake --build tools/shm_web/build
```

生成 `tools/shm_web/build/shm_web_server`，并把 HTML、JS 和字体资源复制到其旁。
从仓库根目录构建时，`ROCOS_SHM_BUILD_WEB` 默认开启，见[根目录构建说明](../../README.md#shared-web-monitor-ecm--igh)。仅需 C++11 公共库时可设置 `-DROCOS_SHM_BUILD_WEB=OFF`。已有构建目录若缓存了 OFF，需显式传入 `-DROCOS_SHM_BUILD_WEB=ON`。

## 用法

**方式一：观察真实主站**（先按 rocos-ecm 或 rocos-igh 各自的说明启动 ABI v2 主站）

```bash
# 与要观察的 ECM 或 IgH 主站使用相同 master_id
./tools/shm_web/build/shm_web_server --master-id 0 --port 8484
```

浏览器打开 `http://127.0.0.1:8484`。一个观察器进程连接一个主站；同时观察 ECM 和 IgH 时，两种主站必须使用不同的 `master_id`，并为观察器指定不同端口。

**方式二：演示模式**（无需硬件 / 主站进程，自建共享内存并模拟数据）

```bash
./tools/shm_web/build/shm_web_server --demo --master-id 926271 --port 8484
```

浏览器打开 `http://127.0.0.1:8484`，会看到一组模拟的伺服驱动 PDO 正在动画（位置斜坡、速度正弦等）。

## 命令行参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--master-id N` | `0` | 要观察的 master_id，决定 `/dev/shm/ecm_v2_{N}` 等命名 |
| `--port P` | `8484` | 监听端口 |
| `--host H` | `127.0.0.1` | 绑定地址；设为 `0.0.0.0` 允许局域网访问 |
| `--demo` | 关 | 自建共享内存 + 模拟数据（无硬件也可看效果） |
| `--help` | - | 打印用法 |

## HTTP 端点

| 路径 | 说明 |
|---|---|
| `GET /` | 前端页面（加载可执行文件旁的 `index.html`） |
| `GET /api/snapshot` | 当前共享内存快照（JSON） |
| `GET /api/events` | SSE 流，约每 250 ms 推送一帧快照 |
| `POST /api/mode` | 显式获取写权限或切回只读 |
| `POST /api/state` | 按能力位提交主站状态请求 |
| `POST /api/command` | SDO 读写 / 周期统计复位；后台收取最终结果 |
| `POST /api/output` | 持有写会话时校验目标后单次写入 OUT / RxPDO |

## 显示内容

依据本仓库的 ABI v2；主站和观察器应使用匹配的公共库版本重新构建：

- `ShmHeader`：magic、ABI 版本、头/总线大小、输入/输出映射长度、ready、实例 ID；保留字节不作业务字段解释。
- `EcatBus`：当前/请求/下一状态、授权、周期与统计、时间戳、主站版本/后端、命令能力、心跳、在线从站数、链路、WKC、DC 状态/指标种类/偏差/采样时间及有效位。
- `Slave`：数组位置与 ID、名称、Vendor ID、Product ID（`product_code`）、修订、序列号、固件、身份来源/有效位、在线/AL 状态/AL 状态码/运行有效位及 PDO 数量。拓扑悬停展示全部这些字段和 PDO 描述/原始字节；节点选择列表提供键盘/触屏入口。
- `PdVar`：名称、对象索引/子索引、偏移、大小和实时字节。1/2/4/8 字节支持整数，4/8 字节支持 FLOAT/DOUBLE，64 位整数用 BigInt 保持精度；其他 1–64 字节字段支持 HEX 原始字节。更长字段仍展示原始字节，但没有写入入口。
- 邮箱：只原子读取 `cmd_box.status`。只有本 Web 所有的事务通过 `executeCommand` / `pollCommand` 获取载荷后，才展示请求与结果（ID、实例、截止时间、目标、字节、结果码、后端错误、SDO abort 和错误有效位）；**不窥读其他 App 的邮箱载荷**。

无效信息显示“未知”并保留原始值，不把配置身份当成在线设备验证。DC 样本超过 3 秒标为过期；不同 `dc_metric_kind` 不混用。实例 ID、命令 ID 和单调时间等 64 位值在 JSON 中用十进制字符串传输。

## 数据流

1. ECM 或 IgH 主站进程创建 `/dev/shm/ecm_v2_{id}`、`pd_input_v2_{id}`、`pd_output_v2_{id}` 与 `sem.sync_v2_{id}_*`。
2. `shm_web_server` 用 `connect(true)` 只读客户端路径 `open` 并 `mmap` 这些对象。
3. 前端用 `EventSource` 订阅 `/api/events`，后端每 250 ms 序列化一次快照推送。
4. 观察器通过 `heartbeat_mono_ns` 是否变化来判断主站存活；主站退出后自动进入"等待"重连状态。

### 默认只读与显式写入

真实主站启动时仅调用 `connect(true)`；演示模式也默认禁止浏览器写入。点击“启用写入”后才建立独立、持久的唯一可写客户端；只读观察连接不消费信号量。

1. 真实总线先停止其他可写 App，在页面勾选“唯一写入者”，再点击“启用写入”。原生 ABI v2 不提供跨 App 的写入者登记，因此 **必须由部署/服务管理器保证外部 App 已停止**。不能靠邮箱空闲来证明没有其他写入者。从已提交过命令的外部 App 接管时，须协调重启主站再启用 Web 写入：ABI v2 没有命令序号交接协议，不能对同一实例从 1 重新发送或盲目补发。
2. 每个总线的 Web 写入进程通过 `/tmp/rocos-shm-web-{id}.lock` 的 `flock` 互斥。锁保持到进程退出，不删除锁文件；它只约束本工具，不替代外部 App 的单例管理。
3. 一次只有一个浏览器会话持有写权限。随机会话令牌不放进快照；请求携带令牌和完整实例 ID。其他页面保持只读。令牌存于该标签页的 `sessionStorage`，刷新后可继续使用；关闭标签页前应切回只读。
4. “切回只读”立即禁止新写入。已有命令仍由后台线程收取结果，可写客户端及其递增命令 ID 保留到本进程退出。切换不会取消、清空或重发邮箱事务，也不会改变已写入的 RxPDO。
5. 心跳过期或主站未就绪时撤销写权限，重连后需重新启用。若断线时命令在途，则锁定写入并标记结果未确认；需协调停止两端、关闭旧映射后重启。持有写客户端的 Web 进程重启也应与主站协调，不能对同一主站实例重置命令序号。

主站状态控制受 `state_control_supported` 限制（IgH 未实现时禁用）。状态请求只写 `request_state`，是否完成以当前状态为准。SDO 与统计复位受 `supported_cmds` 限制，SDO 的目标是从站**数组位置**，不是显示 ID；容量/数据长度为 1–64 B，后端负责实时身份、在线/AL 状态等校验。未支持的 CMD_RESET_FAULT 不提供入口。

命令使用一个在途事务，界面区分成功、确认失败和结果未知；HTTP 返回“已提交”不表示设备已执行。后台独立于 SSE/页面持续 `pollCommand`，包括浏览器已关闭的情况。本地等待超时不重置邮箱，终态 TIMEOUT_UNKNOWN 也不当作确认失败；不自动重试写入。

演示模式自建对象，模拟状态请求、PDO 字典上的 SDO 读写和统计复位；未实现的字典返回 SDO abort。owner 锁阻止覆盖同 ID 的活跃主站，不预先删除任何同名对象。离线验证请使用独立 ID（例如 `--demo --master-id 926271`），不要使用生产 ID。

详见 [ABI v2 规范](../../docs/abi-v2.md)和[部署约定](../../README.md#deployment-contract)。HTTP 接口沿用本机默认监听与同源检查，不提供用户登录认证；远程访问须由部署环境限制访问者。

### CiA 402 Status Word 标签

自动识别 `0x6041:00`（界面显示 `6041.0`），从原始 2 字节小端数据解析，
在实时值后显示驱动器状态：未准备好上电、禁止上电、准备好上电、已上电、运行已使能、
快速停机中、故障反应中或故障；不匹配标准状态编码时显示“未知状态”。
故障用红色、快速停机用黄色、运行使能用绿色区分，同时保留文字。

仅根据 bit 0～3、bit 5～6 匹配状态，每个状态字只显示一个状态标签。
图中 `x` 位不参与判断：未准备好上电、禁止上电、故障反应中和故障使用 `0x004F` 掩码，
其余四种状态使用 `0x006F` 掩码。忽略 bit 4 和 bit 7～15，不显示附加位标签。
例如 `08 12`（`0x1208`）仅显示“故障”。
标签随快照刷新，切换 UINT16/INT16 不影响状态解析。

状态掩码和位定义参考 [CiA 402 Statusword 设备文档](https://doc.synapticon.com/circulo/sw5.1/objects_html/6xxx/6041.html)。

前端回归测试（需要 Node.js 20+）：

```bash
node --test tools/shm_web/*.test.mjs
```


### OUT / RxPDO 字典写入

OUT 行提供输入框和“写入”按钮（也可在输入框按 Enter）。先选择数据类型，再输入数值：

- 整数支持十进制和 `0x` 十六进制；有符号类型可输入负数。
- `FLOAT` 支持小数和科学计数法；所有类型均检查格式和范围，拒绝越界截断。
- 支持 1–64 字节的已映射 OUT 字典；IN 和 `0000.0` 填充项没有写入入口。
- 真实与演示模式均需先启用写入。点击后将小端字节**写入一次** `pd_output_v2_{master_id}`；不持续锁定数值。
  页面提示的是共享内存写入结果，不代表驱动器已经执行。实时快照用于查看后续值。
- 刷新实时数据不会覆盖正在输入的内容；请求期间禁止重复提交，断线时禁用写入。
- 演示模式第一次写入后停止 OUT 动画，保留输出值，IN 动画继续运行。

后端重新核对主站 ID、从站 ID、OUT 字典索引/子索引、偏移、大小及实际映射边界；
未连接、心跳过期、实例改变、无有效会话或命令在途时拒绝写入。Web 在可写模式下承担唯一可写 App 的职责。

所有 POST 要求 `X-Rocos-Write: 1`、`X-Rocos-Instance: <十进制实例 ID>` 和有效的 `Content-Length`，拒绝浏览器跨来源写入。除获取写权限外，还要求 `X-Rocos-Session: <令牌>`。

- `/api/mode`：请求体 `enable sole-writer`（操作方声明外部写入者已停止）或 `disable`；启用成功返回 `session` 和非秘密的 `owner` 标识。
- `/api/state`：十进制状态值 `1 / 2 / 3 / 4 / 8`。
- `/api/command`：`type position index subindex size hexbytes`；读取用 `1 0 4096 0 4 -`，写入用 `2 0 24698 0 4 78563412`，统计复位用 `3 0 0 0 0 -`。返回私有命令结果，最终状态也由快照持续更新。
- `/api/output`：以下 OUT 格式。
请求体为以空格分隔的 `master_id slave_id index sub_index offset size hexbytes`，
前六项是非负十进制整数，末项是严格匹配字节长度的小端十六进制数据。
成功返回 `{"ok":true}`；失败返回非 2xx 状态及 `{"error":"原因"}`。

后端无硬件测试（匿名映射/socketpair、独立子进程及 PID 派生的隔离共享内存 ID）：

```bash
cmake -S tools/shm_web -B tools/shm_web/build
cmake --build tools/shm_web/build
ctest --test-dir tools/shm_web/build --output-on-failure
```

## 迁移来源

本工具从 `rocos-igh/tools/shm_web` 迁入，迁移前源提交为
`cea059f80f8c41a2a40b69187b29768472ca2d64`。今后源码、静态资源和测试统一在本仓库维护。

## 界面与总线拓扑

界面规范见 [DESIGN.md](../../DESIGN.md)。本地 AlibabaPuHuiTi 提供 400 / 500 / 600 / 700 四档原版字重，覆盖表格、控件与拓扑文字；不使用浏览器合成字重。完整声明位于 `AlibabaPuHuiTi-3.0-LICENSE.txt`，来源和 SHA-256 见 `font-sources.json`。原版四字重共约 20.8 MiB，运行不需要公网字体服务。

拓扑使用本地 [Cytoscape.js](https://js.cytoscape.org/) 3.33.1（MIT），来源及校验见 `vendor/README.md`。不需要 React、npm 构建或在线 CDN。请连同构建目录中的 `vendor/`、字体、许可和 JS 文件一起部署。

**图中虚线表示主从逻辑归属，不是物理电缆连接。** 当前 ABI v2 仅提供主站与从站列表，没有端口邻接、分支或环网数据，不能据此恢复物理布线。拓扑按当前快照更新，从站状态独立判定：

- 绿色：有效 OP；黄色：INIT / PREOP / BOOT / SAFEOP 或从站未全部在线。
- 红色：有效离线、AL 错误、主站链路断开或 WKC 异常。
- 灰色：状态未知或连接中断后的过期数据；不把缺失有效位当成正常。

主站为粗边框并标“主站”，从站标“从站”；颜色旁始终有状态文字。支持拖动画布、滚轮/双指缩放、放大/缩小按钮及适应视图。聚焦画布后可用方向键、加减键和 Home。节点选择列表可查看完整名称及状态，选择从站后“定位 PDO”跳到该从站表格。实时刷新保持视角与选择，结构变化时才重新布局。演示主站提供明确的模拟身份与运行有效位，身份来源标为配置；真实主站缺失有效位时仍显示未知。鼠标悬停从站可查看完整身份、运行信息及 PDO 字节；悬浮框可滚动，Escape 关闭，节点选择列表提供同样的信息。
