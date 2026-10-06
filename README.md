# Token-Pet · 修仙小人

> 一个看起来是桌面宠物的东西：Q 版修仙小人，可以拖动、缩放、打坐、御剑、突破。
> 但它本质是一个 **AI 会话 Token 消耗记录数据库** —— 修为随 token 消耗增长，按境界表突破。

![screenshot](docs/screenshot.png)

## 特性

- **纯 C 桌面宠物**：Win32 + GDI+，单 exe（约 1.4MB），无运行时依赖，内存占用约 10MB
- **多数据源采集**：Claude Code、Codex、OpenCode、Kimi Code、cc-switch（代理流量 + 历史补录）
- **多位置支持**：同一工具可同时采集 Windows 与 WSL 侧数据（Codex 跨位置高水位去重）
- **增量扫描**：字节游标 + 尾指纹 + 语义 ID 幂等去重，首次全量后几乎零开销
- **修炼系统**：修为 = 全部 token（含缓存），境界表可配置（`config/realms.json`）
- **修炼日志面板**：会话记录 / 按工具 / 每日修为 / 数据源 / 来源覆盖 / 统计图，六个页签；统计图默认展示全部历史，按日/周/月展示 Token 用量与人民币花费（按工具堆叠、悬停明细、可自定义起止日期）
- **本地隐私**：只读采集，只存 token 计数、成本、时间、模型、会话标题；不读取、不保存对话正文，不上传

## 架构

```
┌─ Windows ──────────────────────────────────────────────────────┐
│  token-pet.exe (Win32 + GDI+ + SQLite)                         │
│  ├─ 小人窗口：分层透明/置顶/拖动/滚轮缩放/动画/特效/境界名牌      │
│  ├─ 修炼日志面板（Token 记录数据库的可视化）                    │
│  ├─ Win 侧采集：.codex、.claude、opencode.db、cc-switch.db      │
│  ├─ WSL 桥：启动采集器子进程，stdin 下发配置 / stdout 收事件     │
│  └─ data\pet.db —— sessions / items / sources / sync_files      │
└───────────────▲─────────────────────────────────────────────────┘
                │ JSONL 事件流
┌─ WSL ─────────┴─────────────────────────────────────────────────┐
│  pet-collector (gcc 编译，零依赖)                               │
│  ├─ ~/.codex（Codex 会话 jsonl）                                │
│  ├─ ~/.local/share/opencode/opencode.db（OpenCode）             │
│  └─ ~/.kimi-code（Kimi Code）                                   │
└─────────────────────────────────────────────────────────────────┘
```

## 数据源矩阵

| 工具 | 位置 | 解析方式 | 去重键 |
|---|---|---|---|
| Codex | Windows + WSL | `sessions/**/rollout-*.jsonl` 累计 token 事件 | 高水位合并（rollout + 时间戳） |
| OpenCode | Windows + WSL | SQLite `session`/`message` 表，逐消息 | 消息 ID |
| Claude Code | Windows | `projects/**/*.jsonl` 逐条 usage | 消息 UUID |
| Kimi Code | WSL | `wire.jsonl` usage 记录 | 会话 + 行号 |
| cc-switch | Windows | 代理 `proxy_request_logs` + 日聚合缺口补录 | request_id / 日期 |

## 修为与境界

- 修为增量 = `input + output + reasoning + cache_read + cache_write`（全部 token 含缓存），1 token = 1 修为
- 花费按 `config\pricing.json` 价目表重算，单位人民币；cc-switch 当前供应商显示为"门派"
- 境界表（默认，可在 `config/realms.json` 调整）：

| 境界 | 起始修为 | 小台阶 |
|---|---|---|
| 凡人 | 0 | — |
| 炼气 | 10 亿 | 一层 ~ 九层 |
| 筑基 | 100 亿 | 初期/中期/后期/圆满 |
| 金丹 | 1000 亿 | 同上 |
| 元婴 | 1 万亿 | 同上 |
| 化神 | 10 万亿 | 同上 |
| 炼虚 | 100 万亿 | 同上 |
| 合体 | 1000 万亿 | 同上 |
| 大乘 | 1 万万亿 | 同上 |
| 渡劫 | 10 万万亿 | 同上 |

## 构建

**依赖**：Windows 侧 Visual Studio 2022 Build Tools（含 Windows SDK，提供 `cl.exe`/`rc.exe`）；WSL 侧 `gcc` + `make`（SQLite 与 cJSON 已内置源码，无需安装其他库）。

```bat
:: 1. 编译 WSL 采集器（在 WSL 里）
cd wsl-collector && make

:: 2. 编译 Windows 主程序（在项目根目录）
build\build.bat
```

产物：`build\token-pet.exe`。双击运行即可，程序会自动通过 `wsl.exe` 拉起采集器。

## 使用

- **拖动**：左键按住小人移动；**缩放**：滚轮（0.5x ~ 2.5x，锚定鼠标位置）
- **右键 / 托盘菜单**：修炼日志、点击穿透、窗口置顶、开机自启、立即扫描、重新扫描全部
- **修炼日志**：双击小人打开；「数据源」页签可查看所有采集位置与状态；「统计图」按日/周/月查看 Token 与花费，
  默认全部历史，可用日期选择器自定义起止（「重置」回到全部）；面板为普通窗口，可最小化到任务栏
- **花费价目表**：`config\pricing.json` 定义各模型每百万 token 的 input/output/cache_read/cache_write 单价
  （currency 可为 CNY/USD，USD 按 usd_cny 折算；match 为模型名子串，自上而下先具体后泛化，未命中再按工具名匹配，
  最后用 default）。首次运行自动生成占位价，请按官方价格校准；修改后重启生效
- **手动指定数据源位置**：`config\sources.json`（留空 = 自动探测）
  ```json
  { "win": { "codex": "D:\\custom\\.codex" }, "wsl": { "codex": "/home/me/.codex" } }
  ```
- **远程镜像（可选）**：其他机器的 Agent 数据用 `tools/sync_remote_agents.sh` 拉取「过滤镜像」（节点在
  `~/.config/tokenpet/sync_nodes.conf` 配置，参考 `tools/sync_nodes.example.conf`；`--check` 可预览计划）——
  Codex/Kimi 只保留 token 事件行并剔除 base_instructions（约原始体积的 0.3%），Claude 只保留 usage/summary 行，
  OpenCode 在远端用 sqlite3 精简导出（仅会话元数据 + assistant 消息）。支持多节点（含经 WSL 的 `/mnt/c` 读取
  Windows 侧数据），然后在 `config\sources.json` 的 `wsl_extras` 登记，采集器每次扫描时自动读入，
  面板「数据源」显示对应标签：
  ```json
  { "wsl_extras": [ { "tool": "codex", "path": "/home/me/agent-mirror/node1/.codex", "label": "远程 node1" } ] }
  ```
  跨机重复会话由全局去重键自动合并，不会重复计数
- **境界表**：`config\realms.json`，修改后重启生效
- **角色素材**：`assets\character\manifest.json` 控制模式（内程序绘制 / 序列帧），`tools\gen_assets.py` 可用生图 API 重新生成（密钥放在 `tools\pptoken.env`，不入库）

## 目录说明

运行时程序根目录是 exe 所在目录（`build\`）：`build\data\pet.db` 是本体数据库，`build\config\` 存设置与境界表。WSL 侧游标在 `~/.local/share/token-pet/cursors.json`。

## 数据与隐私

- 全程只读打开各工具的数据文件（SQLite 只读连接），不修改、不锁库
- 数据库只保存：token 计数、成本、时间戳、模型名、会话标题、工作目录
- 不解析、不保存任何对话正文；无任何网络上报（生图脚本除外，需手动运行）

## 第三方组件

- [SQLite](https://sqlite.org/) — 公有领域（Public Domain）
- [cJSON](https://github.com/DaveGamble/cJSON) — MIT License

## 版权

本项目采用 **MIT License**（详见 [LICENSE](LICENSE)）。第三方组件许可见上节。
