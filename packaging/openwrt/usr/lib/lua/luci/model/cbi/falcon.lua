-- Copyright 2026 cuihairu <chuihairu@gmail.com>
-- SPDX-License-Identifier: MIT

local sys = require("luci.sys")

m = Map("falcon", translate("Falcon 下载"))
m.description = translate(
	"Falcon 是一个多协议下载引擎（HTTP/FTP/SFTP/WebDAV/Metalink），" ..
	"提供 aria2 兼容的 JSON-RPC 接口。保存并应用后服务自动重启。")

s = m:section(NamedSection, "falcon", "falcon", translate("服务设置"))
s.addremove = false
s.anonymous = true

o = s:option(Flag, "enabled", translate("启用服务"))
o.rmempty = false
o.description = translate("禁用即停止后台进程，配置保留。")

o = s:option(ListValue, "rpc_listen_host", translate("RPC 监听地址"))
o:value("127.0.0.1", translate("仅本机 (127.0.0.1)"))
o:value("0.0.0.0", translate("局域网 (0.0.0.0)"))
o:value("::", translate("全部 (IPv6 + IPv4)"))
o.default = "127.0.0.1"
o.description = translate("开放到局域网即允许其他设备访问 JSON-RPC 端口。")

o = s:option(Value, "rpc_port", translate("RPC 端口"))
o.datatype = "port"
o.default = "6800"

o = s:option(Value, "rpc_secret", translate("RPC 密钥"))
o.password = true
o.description = translate(
	"留空 = 不鉴权（仅建议本机监听时使用）。密码输入框不回显已保存值；" ..
	"保存时留空则清除已有密钥。")

o = s:option(Flag, "allow_origin_all", translate("允许跨域访问"))
o.description = translate("允许任意 Origin 的 Web 页面调用 RPC（AriaNg 等前端需要）。")

o = s:option(Value, "download_dir", translate("下载目录"))
o.default = "/mnt/downloads"
o.description = translate(
	"新任务的默认保存目录（按任务传给 aria2.addUri 的 dir 选项，" ..
	"目录不存在时会自动创建）。路由器 /var 是内存盘，不要放在那里。")

o = s:option(Value, "task_db_path", translate("任务数据库路径"))
o.default = "/etc/falcon/tasks.db"
o.description = translate("任务状态持久化（SQLite），需在可写且掉电不丢的分区。")

o = s:option(Value, "max_concurrent", translate("并发任务数"))
o.datatype = "uinteger"
o.default = "5"

o = s:option(Value, "speed_limit", translate("全局下载限速（字节/秒）"))
o.datatype = "uinteger"
o.default = "0"
o.description = translate("0 = 不限速。")

o = s:option(ListValue, "http_engine", translate("HTTP 引擎"))
o:value("v1", "v1 (libcurl)")
o:value("v2", "v2 (内置引擎)")
o.default = "v1"

o = s:option(Flag, "mcp_enabled", translate("启用 MCP 端点"))
o.description = translate("AI 宿主（Claude Desktop 等）可通过 /mcp 端点驱动下载。")

o = s:option(Value, "log_file", translate("日志文件"))
o.default = ""
o.description = translate(
	"留空则输出到系统日志（状态页的\"最近日志\"读 logread）；" ..
	"指定文件后日志不再进 syslog。")

-- Applied changes take effect via a full restart (the rendered
-- /var/etc/falcon-daemon.json is regenerated in start_service); the procd
-- UCI reload trigger (service_triggers) covers changes made outside the
-- web UI. Disabling stops the daemon deterministically.
local apply = luci.http.formvalue("cbi.apply")
if apply then
	local uci = require("luci.model.uci").cursor()
	if uci:get("falcon", "falcon", "enabled") == "1" then
		sys.call("/etc/init.d/falcon restart >/dev/null 2>&1")
	else
		sys.call("/etc/init.d/falcon stop >/dev/null 2>&1")
	end
end

return m
