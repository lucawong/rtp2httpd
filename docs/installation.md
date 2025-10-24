# 安装方式

rtp2httpd 支持多种安装方式，适应不同的使用场景。

## OpenWrt 路由器部署

OpenWrt 是 rtp2httpd 的最佳运行环境。在完成 IPTV 网络融合后（可以搜索教程 `OpenWrt IPTV 融合`），通过 DHCP 获取到 IPTV 内网 IP，可直接访问整个 IPTV 网络，无需 NAT 穿透。

本项目支持的最低 OpenWrt 版本为 21.02，在更低版本上 LuCI 配置界面可能无法加载，但通过手动编辑 `/etc/config/rtp2httpd` 文件并运行 `/etc/init.d/rtp2httpd restart` 仍然可以使用。

### 一键安装脚本

使用一键安装脚本自动下载并安装最新版本。如果你已经安装了 rtp2httpd，重新运行脚本也可以一键更新到最新版。

```bash
curl -fsSL https://raw.githubusercontent.com/stackia/rtp2httpd/main/scripts/install-openwrt.sh | sh
```

脚本会自动：

- 检测设备的 CPU 架构
- 从 GitHub Release 获取最新版本
- 下载并安装所有必需的软件包（主程序 + LuCI 界面 + 语言包）

### 手动安装

如果无法使用一键脚本，可以手动在 [Releases](https://github.com/stackia/rtp2httpd/releases) 页面下载对应架构的软件包：

- `rtp2httpd_x.y.z-1_<arch>.ipk` - 主程序包
- `luci-app-rtp2httpd_x.y.z_all.ipk` - LuCI Web 界面
- `luci-i18n-rtp2httpd-en_x.y.z_all.ipk` - 英文语言包
- `luci-i18n-rtp2httpd-zh-cn_x.y.z_all.ipk` - 中文语言包

```bash
opkg install rtp2httpd_*.ipk luci-app-rtp2httpd_*.ipk luci-i18n-rtp2httpd-*.ipk
```

## 静态二进制文件部署

从 [Releases](https://github.com/stackia/rtp2httpd/releases) 页面下载对应架构的静态二进制文件 `rtp2httpd-<版本号>-<架构>`，上传到设备并 `chmod +x` 后即可运行。

默认从 `/etc/rtp2httpd.conf` 读取配置文件。可用 `--config` 或 `--noconfig` 参数覆盖。

**示例**：

```bash
# 下载二进制文件
wget https://github.com/stackia/rtp2httpd/releases/download/vX.Y.Z/rtp2httpd-X.Y.Z-x86_64
chmod +x rtp2httpd-X.Y.Z-x86_64

# 使用配置文件运行
./rtp2httpd-X.Y.Z-x86_64 --config /path/to/rtp2httpd.conf

# 仅使用命令行参数运行
./rtp2httpd-X.Y.Z-x86_64 --noconfig --verbose 2 --listen 5140 --maxclients 20
```

## Docker 容器部署

适用于支持 Docker 的设备。**必须使用 host 网络模式**以接收组播流。

### 重要说明

**⚠️ 启用零拷贝时必须添加 `--ulimit memlock=-1:-1` 参数**

如果你通过 `--zerocopy-on-send` 参数启用了零拷贝发送，MSG_ZEROCOPY 技术需要锁定内存页。Docker 容器默认的 locked memory 限制（64KB）太小，会导致 ENOBUFS 错误，表现为：

- 客户端无法播放
- 服务端 buffer pool 疯狂增长
- 统计数字中的 ENOBUFS 错误飙升

默认情况下 rtp2httpd 不启用零拷贝，因此不需要 `--ulimit memlock=-1:-1` 参数。只有在你明确启用零拷贝发送时才需要添加此参数。

### 正确的启动方式

```bash
docker run --network=host --ulimit memlock=-1:-1 --rm \
  ghcr.io/stackia/rtp2httpd:latest \
  --noconfig --verbose 2 --listen 5140 --maxclients 20
```

### 使用 docker-compose

```yaml
services:
  rtp2httpd:
    image: ghcr.io/stackia/rtp2httpd:latest
    network_mode: host
    ulimits:
      memlock:
        soft: -1
        hard: -1
    command: --noconfig --verbose 2 --listen 5140 --maxclients 20
```

### 挂载配置文件

如果需要使用配置文件：

```bash
docker run --network=host --ulimit memlock=-1:-1 --rm \
  -v /path/to/rtp2httpd.conf:/etc/rtp2httpd.conf:ro \
  ghcr.io/stackia/rtp2httpd:latest
```

## 编译安装

适用于需要自定义编译或开发调试的场景。

### 安装依赖

```bash
# Ubuntu/Debian
sudo apt-get install build-essential autoconf automake pkg-config curl

# 安装 Node.js LTS（用于构建 Web UI）
curl -fsSL https://deb.nodesource.com/setup_22.x | sudo -E bash -
sudo apt-get install -y nodejs
```

### 编译步骤

```bash
# 克隆仓库
git clone https://github.com/stackia/rtp2httpd.git
cd rtp2httpd

# 构建前端并嵌入静态资源
npm ci --prefix web-ui
npm run build --prefix web-ui
node scripts/embed-status-page.js web-ui/dist/index.html src/status_page.h

# 生成构建脚本
autoreconf -fi

# 配置和编译
./configure --enable-optimization=-O3
make

# 安装
sudo make install
```

### 可选依赖

- **curl**：用于获取外部 M3U 播放列表（HTTP/HTTPS）
- **ffmpeg**：用于视频快照功能

## 下一步

- [快速上手](quick-start.md)：OpenWrt 快速配置指南
- [配置参数详解](configuration.md)：了解所有配置选项
