-- Copyright 2026 cuihairu <chuihairu@gmail.com>
-- SPDX-License-Identifier: MIT

module("luci.controller.falcon", package.seeall)

function index()
	if not nixio.fs.access("/etc/config/falcon") then
		return
	end

	entry({"admin", "services", "falcon"},
		cbi("falcon"),
		_("Falcon 下载"), 60).dependent = true

	entry({"admin", "services", "falcon", "panel"},
		template("falcon/status"),
		_("传输面板"), 10).leaf = true

	-- Internal API routes: .leaf = true keeps them out of the menu
	-- (custos 44a29e5 lesson).
	entry({"admin", "services", "falcon", "api", "status"},
		call("api_status")).leaf = true

	entry({"admin", "services", "falcon", "api", "add"},
		call("api_add")).leaf = true

	entry({"admin", "services", "falcon", "api", "control"},
		call("api_control")).leaf = true
end

-- Strip control characters and cap the length. The page renders through
-- DOM textContent so HTML injection is impossible; this keeps JSON
-- payloads and table rows bounded (gids/paths/log lines are untrusted
-- remote data).
local function sanitize(value, max)
	if value == nil then
		return ""
	end
	local s = tostring(value):gsub("%c", "")
	if #s > max then
		s = s:sub(1, max) .. "…"
	end
	return s
end

local function shell_quote(s)
	return "'" .. tostring(s):gsub("'", "'\\''") .. "'"
end

local function uci_falcon()
	return require("luci.model.uci").cursor()
end

local function rpc_url(uci)
	local host = uci:get("falcon", "falcon", "rpc_listen_host") or "127.0.0.1"
	local port = uci:get("falcon", "falcon", "rpc_port") or "6800"
	-- A wildcard listen address still loops back locally.
	if host == "" or host == "0.0.0.0" or host == "::" then
		host = "127.0.0.1"
	end
	return string.format("http://%s:%s/jsonrpc", host, port)
end

-- One JSON-RPC call through the daemon's aria2-compatible HTTP face
-- (existing RPC surface only; no new engine-side interface). Token auth
-- rides in params[0] when a secret is configured. Returns result, nil on
-- success or nil, error message.
local function rpc(method, params)
	local uci = uci_falcon()
	local secret = uci:get("falcon", "falcon", "rpc_secret") or ""
	local full = {}
	if secret ~= "" then
		full[#full + 1] = "token:" .. secret
	end
	for _, p in ipairs(params or {}) do
		full[#full + 1] = p
	end
	local body = luci.jsonc.stringify({
		jsonrpc = "2.0", id = 1, method = method, params = full })
	local cmd = string.format(
		"curl -s -m 8 -X POST -H 'Content-Type: application/json' -d %s %s 2>/dev/null",
		shell_quote(body), shell_quote(rpc_url(uci)))
	local raw = require("luci.util").exec(cmd)
	if raw == nil or raw == "" then
		return nil, "daemon 无响应（服务未运行？）"
	end
	local resp = luci.jsonc.parse(raw)
	if resp == nil then
		return nil, "JSON 解析失败"
	end
	if resp.error then
		return nil, sanitize(resp.error.message or tostring(resp.error.code or "?"), 200)
	end
	return resp.result, nil
end

-- aria2 tellStatus JSON: files is a 1-indexed array with a null slot at 0
-- (files[1] == null), so ipairs/`#` cannot be trusted on it — scan with
-- pairs for the first real entry. The display name is that file's
-- basename; BitTorrent tasks carry a friendlier bittorrent.info.name (BT
-- is compiled out of the router build, the field is handled anyway).
local function task_name(t)
	if type(t.bittorrent) == "table" and type(t.bittorrent.info) == "table"
		and t.bittorrent.info.name then
		return tostring(t.bittorrent.info.name)
	end
	if type(t.files) == "table" then
		for _, f in pairs(t.files) do
			if type(f) == "table" and f.path then
				local p = tostring(f.path)
				return p:match("[^/\\]+$") or p
			end
		end
	end
	return tostring(t.gid or "")
end

local function task_row(t, fallback_status)
	return {
		gid = sanitize(t.gid, 32),
		name = sanitize(task_name(t), 120),
		status = sanitize(t.status or fallback_status, 16),
		total = tonumber(t.totalLength) or 0,
		completed = tonumber(t.completedLength) or 0,
		speed = tonumber(t.downloadSpeed) or 0,
		upload = tonumber(t.uploadSpeed) or 0,
	}
end

-- Four-card data source: procd service state, global counters
-- (aria2.getGlobalStat), the transfer list (tellActive + tellWaiting —
-- paused/queued jobs must show their 继续 button), and recent daemon log
-- lines (procd captures the daemon's stdout/stderr into syslog).
function api_status()
	luci.http.prepare_content("application/json")
	local sys = require("luci.sys")

	local code = sys.call("/etc/init.d/falcon status >/dev/null 2>&1")
	local out = {
		status = (code == 0) and "running" or "stopped",
		tasks = {},
	}

	if out.status == "running" then
		local stat, err = rpc("aria2.getGlobalStat")
		if err then
			out.rpc_error = err
		else
			out.global = {
				download_speed = tonumber(stat.downloadSpeed) or 0,
				upload_speed = tonumber(stat.uploadSpeed) or 0,
				num_active = tonumber(stat.numActive) or 0,
				num_waiting = tonumber(stat.numWaiting) or 0,
				num_stopped = tonumber(stat.numStoppedTotal or stat.numStopped) or 0,
			}
		end

		local keys = { "gid", "status", "totalLength", "completedLength",
			"downloadSpeed", "uploadSpeed", "files", "bittorrent" }

		local active, aerr = rpc("aria2.tellActive", { keys })
		if aerr then
			out.rpc_error = out.rpc_error or aerr
		else
			for _, t in ipairs(active or {}) do
				out.tasks[#out.tasks + 1] = task_row(t, "active")
			end
		end

		local waiting, werr = rpc("aria2.tellWaiting", { 0, 100, keys })
		if werr then
			out.rpc_error = out.rpc_error or werr
		else
			for _, t in ipairs(waiting or {}) do
				out.tasks[#out.tasks + 1] = task_row(t, t.status or "waiting")
			end
		end
	end

	local logs = {}
	local raw = require("luci.util").exec("logread -e falcon 2>/dev/null | tail -n 25")
	for line in (raw or ""):gmatch("[^\r\n]+") do
		logs[#logs + 1] = sanitize(line, 200)
	end
	out.log = logs

	luci.http.write_json(out)
end

-- Add a download task (URL -> aria2.addUri). The target directory comes
-- from the UCI download_dir and is passed per task as the daemon's "dir"
-- option (download_dir is not a daemon.json field); created on demand so
-- a fresh install just works.
function api_add()
	luci.http.prepare_content("application/json")
	luci.dispatcher.test_post_security()

	local payload = luci.http.formvalue("payload")
	local req = payload and payload ~= "" and luci.jsonc.parse(payload) or nil
	if type(req) ~= "table" then
		luci.http.write_json({ ok = false, error = "请求格式错误" })
		return
	end

	local url = sanitize(req.url, 2048)
	if url == ""
		or (not url:match("^[%w%+%-%.]+://") and not url:match("^magnet:")) then
		luci.http.write_json({ ok = false, error = "URL 格式无效" })
		return
	end

	local uci = uci_falcon()
	local dir = uci:get("falcon", "falcon", "download_dir") or "/mnt/downloads"
	os.execute("mkdir -p " .. shell_quote(dir))

	local opts = { dir = dir }
	if type(req.out) == "string" and req.out ~= "" then
		local out_name = sanitize(req.out, 255)
		-- Only a bare filename is accepted; directory pieces are the dir
		-- option's job.
		if not out_name:match("[/\\]") and out_name ~= "." and out_name ~= ".." then
			opts.out = out_name
		end
	end

	local gid, err = rpc("aria2.addUri", { { url }, opts })
	if err then
		luci.http.write_json({ ok = false, error = err })
		return
	end
	luci.http.write_json({
		ok = true,
		gid = sanitize(type(gid) == "table" and gid.gid or gid, 32),
	})
end

-- pause / unpause / remove on one task.
function api_control()
	luci.http.prepare_content("application/json")
	luci.dispatcher.test_post_security()

	local payload = luci.http.formvalue("payload")
	local req = payload and payload ~= "" and luci.jsonc.parse(payload) or nil
	local op = type(req) == "table" and tostring(req.op) or ""
	local gid = type(req) == "table" and sanitize(req.gid, 32) or ""

	if not gid:match("^[0-9a-fA-F]+$") then
		luci.http.write_json({ ok = false, error = "gid 无效" })
		return
	end

	local method
	if op == "pause" then
		method = "aria2.pause"
	elseif op == "unpause" then
		method = "aria2.unpause"
	elseif op == "remove" then
		method = "aria2.remove"
	else
		luci.http.write_json({ ok = false, error = "未知操作" })
		return
	end

	local _, err = rpc(method, { gid })
	if err then
		luci.http.write_json({ ok = false, error = err })
		return
	end
	luci.http.write_json({ ok = true })
end
