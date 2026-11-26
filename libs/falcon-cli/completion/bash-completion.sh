# falcon bash completion script

_falcon_completion() {
    local cur prev words cword
    _init_completion || return

    # 主命令
    local commands="download add-remove pause purge remove toggle version help"

    # 全局选项
    local global_options="-h --help -v --version --quiet --no-warn"

    # 下载选项
    local download_options="-d --dir -o --output -s --split --checksum
                          --summary-interval --timeout --max-tries
                          --http-user --http-passwd --http-proxy
                          --https-proxy --ftp-user --ftp-passwd
                          --ftp-pasv --ftp-type --lowest-speed-limit
                          --max-download-limit --stream-piece-selector
                          --allow-overwrite --allow-piece-length-change
                          --auto-file-renaming --file-allocation
                          --dry-run --rpc-allow-origin-all"

    # BitTorrent 选项
    local bt_options="--bt-enable-lpd --bt-enable-dht --bt-enable-tracker
                      --bt-exclude-tracker --bt-max-peers --bt-min-crypto-level
                      --bt-require-crypto --bt-request-peer-speed-limit
                      --bt-max-upload-limit --bt-seed-unverified
                      --bt-save-metadata --bt-remove-unselected-file
                      --bt-detach-seed-only --bt-detach-seed-only
                      --bt-stop-timeout --bt-prioritize-piece-head
                      --bt-tracker-connect-timeout --bt-tracker-timeout
                      --bt-tracker-interval --peer-id-prefix
                      --seed-ratio --seed-time --follow-torrent
                      --follow-metalink --no-proxy --index-out
                      --select-file --bt-external-ip --on-download-start
                      --on-download-stop --on-download-complete
                      --on-download-error --on-bt-download-complete"

    # RPC 选项
    local rpc_options="--rpc-listen-port --rpc-max-request-size
                       --rpc-secret --rpc-allow-origin-all
                       --rpc-listen-all --rpc-save-upload-metadata
                       --rpc-secure --rpc-certificate --rpc-private-key
                       --rpc-allow-origin-all --rpc-user --rpc-passwd"

    # 根据当前命令显示补全
    case "${prev}" in
        download|add)
            COMPREPLY=($(compgen -W "${download_options} ${bt_options}" -- "${cur}"))
            ;;
        pause|remove)
            # 补全 GID
            local gids=$(falcon --show-gids 2>/dev/null)
            COMPREPLY=($(compgen -W "${gids}" -- "${cur}"))
            ;;
        -d|--dir|-o|--output)
            COMPREPLY=($(compgen -d -- "${cur}"))
            ;;
        -h|--help)
            COMPREPLY=($(compgen -W "${commands}" -- "${cur}"))
            ;;
        *)
            if [[ ${cword} -eq 1 ]]; then
                COMPREPLY=($(compgen -W "${commands} ${global_options}" -- "${cur}"))
            else
                case "${words[1]}" in
                    download|add)
                        COMPREPLY=($(compgen -W "${download_options} ${bt_options}" -- "${cur}"))
                        ;;
                    pause|remove)
                        local gids=$(falcon --show-gids 2>/dev/null)
                        COMPREPLY=($(compgen -W "${gids}" -- "${cur}"))
                        ;;
                    purge)
                        COMPREPLY=($(compgen -W "[force]" -- "${cur}"))
                        ;;
                esac
            fi
            ;;
    esac

    # 文件名补全
    if [[ "${cur}" == *://* ]] || [[ "${cur}" == *.torrent ]] || [[ "${cur}" == *.metalink ]]; then
        _filedir
    fi
} &&
complete -F _falcon_completion falcon

# 如果是 falcon-cli 别名
complete -F _falcon_completion falcon-cli
