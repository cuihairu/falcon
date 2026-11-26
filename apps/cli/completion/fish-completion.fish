# falcon fish completion script

function __falcon_commands
    echo -e "download\tDownload URIs"
    echo -e "add\tAdd download task"
    echo -e "remove\tRemove download task"
    echo -e "pause\tPause download"
    echo -e "unpause\tUnpause download"
    echo -e "purge\tPurge completed/failed downloads"
    echo -e "toggle\tToggle pause/resume"
    echo -e "version\tPrint version"
    echo -e "help\tPrint help"
end

function __falcon_gids
    falcon --show-gids 2>/dev/null
end

function __falcon_no_subcommand
    set -l cmd (commandline -opc)
    if [ (count $cmd) -eq 1 ]
        echo -e "commands\tSubcommands"
        __falcon_commands
    end
end

function __falcon_using_command
    set -l cmd (commandline -opc)
    [ (count $cmd) -gt 1 ]; and echo $cmd[2]
end

function __falcon_download_options
    echo -e "-d\tSet directory"
    echo -e "--dir\tSet directory"
    echo -e "-o\tSet output file name"
    echo -e "--output\tSet output file name"
    echo -e "-s\tSplit download"
    echo -e "--split\tSplit download"
    echo -e "--checksum\tVerify checksum"
    echo -e "--summary-interval\tSet summary interval"
    echo -e "--timeout\tSet timeout"
    echo -e "--max-tries\tSet max retries"
    echo -e "--http-user\tSet HTTP user"
    echo -e "--http-passwd\tSet HTTP password"
    echo -e "--http-proxy\tSet HTTP proxy"
    echo -e "--https-proxy\tSet HTTPS proxy"
    echo -e "--ftp-user\tSet FTP user"
    echo -e "--ftp-passwd\tSet FTP password"
    echo -e "--ftp-pasv\tUse PASV mode"
    echo -e "--ftp-type\tSet FTP type"
    echo -e "--lowest-speed-limit\tSet lowest speed limit"
    echo -e "--max-download-limit\tSet max download speed"
    echo -e "--allow-overwrite\tAllow overwriting"
    echo -e "--auto-file-renaming\tRename if file exists"
    echo -e "--file-allocation\tSet file allocation method"
    echo -e "--dry-run\tPerform dry run"
    echo -e "--bt-enable-lpd\tEnable Local Peer Discovery"
    echo -e "--bt-enable-dht\tEnable DHT"
    echo -e "--bt-enable-tracker\tEnable tracker"
    echo -e "--bt-exclude-tracker\tExclude tracker"
    echo -e "--bt-max-peers\tSet max peers"
    echo -e "--bt-min-crypto-level\tSet min crypto level"
    echo -e "--bt-require-crypto\tRequire crypto"
    echo -e "--bt-request-peer-speed-limit\tSet request peer speed limit"
    echo -e "--bt-max-upload-limit\tSet max upload speed"
    echo -e "--bt-seed-unverified\tSeed unverified"
    echo -e "--bt-save-metadata\tSave metadata"
    echo -e "--bt-remove-unselected-file\tRemove unselected files"
    echo -e "--seed-ratio\tSet seed ratio"
    echo -e "--seed-time\tSet seed time"
    echo -e "--follow-torrent\tFollow torrent"
    echo -e "--follow-metalink\tFollow metalink"
    echo -e "--index-out\tSet file output"
    echo -e "--select-file\tSelect file to download"
end

complete -c falcon -f

# 主命令补全
complete -c falcon -n __falcon_no_subcommand -a "(__falcon_commands)"

# 下载命令选项
complete -c falcon -n "__falcon_using_command download" -a "(__falcon_download_options)"
complete -c falcon -n "__falcon_using_command add" -a "(__falcon_download_options)"

# GID 补全
complete -c falcon -n "__falcon_using_command pause" -a "(__falcon_gids)"
complete -c falcon -n "__falcon_using_command remove" -a "(__falcon_gids)"
complete -c falcon -n "__falcon_using_command unpause" -a "(__falcon_gids)"

# 目录补全
complete -c falcon -n "__falcon_using_command download" -l dir -d "Directory"
complete -c falcon -n "__falcon_using_command download" -l output -d "File"

# 帮助选项
complete -c falcon -l help -d "Show help"
complete -c falcon -l version -d "Show version"
complete -c falcon -l quiet -d "Disable output"
complete -c falcon -l no-warn -d "Disable warnings"
