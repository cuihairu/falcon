# Shell 自动补全脚本

## 安装说明

### Bash

#### 系统-wide 安装
```bash
sudo cp bash-completion.sh /usr/share/bash-completion/completions/falcon
sudo cp bash-completion.sh /usr/share/bash-completion/completions/falcon-cli
```

#### 用户级别安装
```bash
mkdir -p ~/.local/share/bash-completion/completions
cp bash-completion.sh ~/.local/share/bash-completion/completions/falcon
cp bash-completion.sh ~/.local/share/bash-completion/completions/falcon-cli
```

#### 临时启用
```bash
source bash-completion.sh
```

### Zsh

#### 系统-wide 安装
```bash
sudo cp zsh-completion.zsh /usr/share/zsh/site-functions/_falcon
sudo cp zsh-completion.zsh /usr/share/zsh/site-functions/_falcon-cli
```

#### 用户级别安装
```bash
mkdir -p ~/.zsh/completion
cp zsh-completion.zsh ~/.zsh/completion/_falcon
cp zsh-completion.zsh ~/.zsh/completion/_falcon-cli

# 添加到 ~/.zshrc
echo "fpath=(~/.zsh/completion \$fpath)" >> ~/.zshrc
echo "autoload -U compinit && compinit" >> ~/.zshrc
```

#### 临时启用
```zsh
autoload -U compinit && compinit
source zsh-completion.zsh
```

### Fish

#### 系统-wide 安装
```bash
sudo cp fish-completion.fish /usr/share/fish/vendor_completions.d/falcon.fish
sudo cp fish-completion.fish /usr/share/fish/vendor_completions.d/falcon-cli.fish
```

#### 用户级别安装
```bash
mkdir -p ~/.config/fish/completions
cp fish-completion.fish ~/.config/fish/completions/falcon.fish
cp fish-completion.fish ~/.config/fish/completions/falcon-cli.fish
```

#### 临时启用
```fish
source fish-completion.fish
```

## 使用示例

### 命令补全
```bash
falcon <TAB>
# 显示所有可用命令
```

### 选项补全
```bash
falcon download --<TAB>
# 显示所有下载选项
```

### GID 补全
```bash
falcon pause <TAB>
# 显示当前活动的 GID
```

### 目录补全
```bash
falcon download -d <TAB>
# 显示目录列表
```

## 支持的命令

| 命令 | 说明 |
|------|------|
| `download` | 下载文件 |
| `add` | 添加下载任务 |
| `remove` | 移除下载任务 |
| `pause` | 暂停下载 |
| `unpause` | 恢复下载 |
| `purge` | 清理已完成/失败的任务 |
| `toggle` | 切换暂停/恢复 |
| `version` | 显示版本信息 |
| `help` | 显示帮助信息 |

## 支持的选项

### 下载选项
- `-d, --dir`: 设置下载目录
- `-o, --output`: 设置输出文件名
- `-s, --split`: 分块下载数
- `--timeout`: 超时时间
- `--max-tries`: 最大重试次数
- `--max-download-limit`: 最大下载速度
- `--allow-overwrite`: 允许覆盖
- `--auto-file-renaming`: 自动重命名

### BitTorrent 选项
- `--bt-enable-dht`: 启用 DHT
- `--bt-enable-lpd`: 启用本地节点发现
- `--bt-max-peers`: 最大连接数
- `--seed-ratio`: 分享比率
- `--seed-time`: 分享时间
- `--select-file`: 选择文件

### RPC 选项
- `--rpc-listen-port`: RPC 端口
- `--rpc-user`: RPC 用户名
- `--rpc-passwd`: RPC 密码
- `--rpc-secret`: RPC 密钥

## 测试补全

安装完成后，重新打开终端或重新加载配置：

```bash
# Bash
source ~/.bashrc

# Zsh
source ~/.zshrc

# Fish
source ~/.config/fish/config.fish
```

然后测试补全：

```bash
falcon <TAB>                     # 测试命令补全
falcon download --<TAB>          # 测试选项补全
falcon pause <TAB>               # 测试 GID 补全
falcon download -d <TAB>         # 测试目录补全
```

## 注意事项

1. 补全脚本需要 `falcon` 可执行文件在 PATH 中
2. 某些高级补全功能需要 falcon 支持 `--show-gids` 选项
3. 如果补全不生效，请检查：
   - 文件权限是否正确
   - 文件位置是否在补全搜索路径中
   - 是否重新加载了 shell 配置

## 故障排除

### Bash 补全不生效

确保 `bash-completion` 已安装：
```bash
# Debian/Ubuntu
sudo apt-get install bash-completion

# CentOS/RHEL
sudo yum install bash-completion

# macOS
brew install bash-completion
```

### Zsh 补全不生效

确保启用了补全系统：
```zsh
autoload -U compinit
compinit
```

### Fish 补全不生效

Fish 的补全应该是自动的。如果不行，检查文件是否在正确的目录中：
```fish
echo $__fish_data_dir/completions
```
