<p align="right">
  <a href="local-dev-workflow.md">English</a> · <strong>简体中文</strong>
</p>

# 本地开发流程：提交、构建、刷机、抓日志

本分支应用开发的强制本地流程。每个批准的改动集按以下顺序执行，不得跳步。

## 1. 每个改动集完成即提交

- 改动集完成并通过验证后，立即在当前特性分支提交，不让成品代码滞留在工作区。
- 提交信息用英文、约定式风格（`feat(scope): ...`、`fix(scope): ...`）。
- 永不提交 `build/`、`sdkconfig`、`managed_components/`、`.cache/`、
  `__pycache__/`（已在 `.gitignore`）。

## 2. 提交后构建固件

工具链不在默认 `python3` 探测路径上，必须显式固定 Python 环境：

```bash
export IDF_PYTHON_ENV_PATH=/home/apps/.espressif/python_env/idf5.5_py3.11_env
source /home/apps/esp/esp-idf/export.sh
cd /workspace/apps/empty-GPWS/ai-passport
idf.py build          # 完整门禁可用: ./tools/validate.sh --firmware
```

## 3. 汇报固件地址与刷机命令

每次构建后向用户汇报：

- **固件路径**：`build/FoloToy-AI-Passport-full.bin`（已验证的合并镜像：
  bootloader + 分区表 + 应用；由 `./tools/validate.sh --firmware` 产出）
- **刷机命令**（按实际连接端口，通常 `/dev/ttyACM0`、`/dev/ttyUSB0`，
  macOS 为 `/dev/cu.usbmodem*`）：

```bash
# 首选：单文件已验证合并镜像，0x0 一次烧写（见 environment-setup.zh_CN.md）
python3 -m esptool --chip esp32c3 -b 460800 \
  --before default_reset --after hard_reset \
  --port <PORT> \
  write_flash 0x0 build/FoloToy-AI-Passport-full.bin

# 仅增量开发烧录（保留已有 NVS 状态）：
idf.py -p <PORT> flash
```

规则：

- 首选合并镜像：一次烧写、无手工逐镜像偏移、且烧的是固件门禁验证过的产物。
  三镜像分段 esptool 命令把偏移手写在命令里、烧的是未经验证的中间产物，
  不作为默认方式。
- 合并镜像可能重置镜像间隙的 NVS/PHY 区域；需要保留已有 NVS 状态时改用
  分段 `idf.py flash`。
- 刷机仍须先获得用户明确同意（遵循仓库 AGENTS.md 的真机测试政策）。
- 不要自行改波特率或去掉 `--before default_reset --after hard_reset`：
  460800 是 USB-Serial/JTAG 口验证过的速率；更高速率（如 921600）会在
  烧写中途掉线。
- **刷机前必须先停日志抓取（第 4 节）。** 并发的串口读取会吃掉 esptool 的
  SLIP 回包，烧写中途以 `chip stopped responding` 中断（设备仍可用重刷救回）。

## 4. 用 `tools/bootlog.py` 抓设备日志

标准日志抓取工具，收编在仓库内版本化，不会再丢。在设备旁的主机上运行
（macOS 或 Linux）；只依赖 `pyserial`（esptool 已附带）。

```bash
cd /workspace/apps/empty-GPWS/ai-passport   # 或 Mac 上的仓库副本

# 启动（前台；Ctrl+C 停止）
python3 tools/bootlog.py                     # 默认: /dev/cu.usbmodem* -> boot.log
python3 tools/bootlog.py -p /dev/ttyUSB0     # Linux 或精确端口
python3 tools/bootlog.py -p '/dev/cu.usbmodem*' -o run.log

# 后台方式
nohup python3 tools/bootlog.py >/dev/null 2>&1 &
pkill -f tools/bootlog.py                    # 停止
```

行为与规则：

- 自动重连：每次深睡或重启后设备会以可能不同的 USB 名重新枚举，因此传
  glob 通配模式、绝不传固定端口名。工具会持续重扫直到设备回来。
- attach/detach 时间戳写进日志文件。整个复现窗口内保留该文件：即使终端
  错过了，崩溃现场与开机首行的复位原因日志都在文件里。
- 刷机与抓日志互斥：先 `pkill -f tools/bootlog.py`，刷完再重新启动抓取。

## 5. 每个改动集的执行顺序

1. 方案经用户审计批准（批准前不动代码）。
2. 实现最小改动集。
3. `./tools/validate.sh --static`（host 测试）与 `idf.py build`（固件）通过。
4. 提交。
5. 汇报：修改文件、验证结果、固件路径、刷机命令。
