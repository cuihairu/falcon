#compdef falcon

# falcon zsh completion script

_falcon() {
    local -a commands
    commands=(
        'download:Download URIs'
        'add:Add download task'
        'remove:Remove download task'
        'pause:Pause download'
        'unpause:Unpause download'
        'purge:Purge completed/failed downloads'
        'toggle:Toggle pause/resume'
        'version:Print version'
        'help:Print help'
    )

    local -a download_options
    download_options=(
        '-d[Set directory]:directory:_directories'
        '--dir[Set directory]:directory:_directories'
        '-o[Set output file name]:filename:_files'
        '--output[Set output file name]:filename:_files'
        '-s[Split download]:number'
        '--split[Split download]:number'
        '--checksum[Verify checksum]'
        '--summary-interval[Set summary interval]:seconds'
        '--timeout[Set timeout]:seconds'
        '--max-tries[Set max retries]:number'
        '--http-user[Set HTTP user]:user'
        '--http-passwd[Set HTTP password]:password'
        '--http-proxy[Set HTTP proxy]:url:_urls'
        '--https-proxy[Set HTTPS proxy]:url:_urls'
        '--ftp-user[Set FTP user]:user'
        '--ftp-passwd[Set FTP password]:password'
        '--ftp-pasv[Use PASV mode]'
        '--ftp-type[Set FTP type]:type:(binary ascii)'
        '--lowest-speed-limit[Set lowest speed limit]:speed'
        '--max-download-limit[Set max download speed]:speed'
        '--allow-overwrite[Allow overwriting]'
        '--auto-file-renaming[Rename if file exists]'
        '--file-allocation[Set file allocation method]:method:(none prealloc falloc trunc)'
        '--dry-run[Perform dry run]'
        '--rpc-allow-origin-all[Allow all RPC origins]'
    )

    local -a bt_options
    bt_options=(
        '--bt-enable-lpd[Enable Local Peer Discovery]'
        '--bt-enable-dht[Enable DHT]'
        '--bt-enable-tracker[Enable tracker]'
        '--bt-exclude-tracker[Exclude tracker]:url'
        '--bt-max-peers[Set max peers]:number'
        '--bt-min-crypto-level[Set min crypto level]:level:(plain arc4 secure)'
        '--bt-require-crypto[Require crypto]'
        '--bt-request-peer-speed-limit[Set request peer speed limit]:speed'
        '--bt-max-upload-limit[Set max upload speed]:speed'
        '--bt-seed-unverified[Seed unverified]'
        '--bt-save-metadata[Save metadata]'
        '--bt-remove-unselected-file[Remove unselected files]'
        '--seed-ratio[Set seed ratio]:ratio'
        '--seed-time[Set seed time]:time'
        '--follow-torrent[Follow torrent]:(true false mem)'
        '--follow-metalink[Follow metalink]:(true false mem)'
        '--index-out[Set file output]:index:path'
        '--select-file[Select file to download]:files'
    )

    local -a rpc_options
    rpc_options=(
        '--rpc-listen-port[Set RPC port]:port'
        '--rpc-max-request-size[Set max request size]:size'
        '--rpc-secret[Set RPC secret]:secret'
        '--rpc-allow-origin-all[Allow all origins]'
        '--rpc-listen-all[Listen on all interfaces]'
        '--rpc-user[Set RPC user]:user'
        '--rpc-passwd[Set RPC password]:password'
    )

    local -a global_options
    global_options=(
        '-h[Show help]'
        '--help[Show help]'
        '-v[Show version]'
        '--version[Show version]'
        '--quiet[Disable output]'
        '--no-warn[Disable warnings]'
    )

    local curcontext="$curcontext" state line
    typeset -A opt_args

    _arguments -C \
        "${global_options[@]}" \
        '1: :->command' \
        '*::arg:->args' \
        && return 0

    case $state in
        command)
            _describe -t commands 'falcon commands' commands
            ;;
        args)
            case ${words[2]} in
                download|add)
                    _arguments "${download_options[@]}" "${bt_options[@]}"
                    _urls
                    ;;
                pause|remove|unpause)
                    # 补全 GID
                    local gids=($(falcon --show-gids 2>/dev/null))
                    _describe 'GIDs' gids
                    ;;
                purge)
                    _arguments ':force:(force)'
                    ;;
                *)
                    _arguments "${download_options[@]}" "${bt_options[@]}" "${rpc_options[@]}"
                    _urls
                    ;;
            esac
            ;;
    esac
}

_falcon "$@"
