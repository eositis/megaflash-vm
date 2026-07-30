-- license:BSD-3-Clause
-- Forward Apple //c memexp soft-switches ($C0C0-$C0C3) to Bramble's
-- MegaFlash a2bus TCP bridge.
--
-- IMPORTANT: open the TCP socket as a *client* only (READ|WRITE, no CREATE).
-- With CREATE, MAME falls back to listen-on-same-port when connect fails, which
-- yields "Address already in use" while Bramble already owns the port — taps
-- then run with sock=nil and MegaFlash is never seen.

	local exports = {
	name = "megaflash_bridge",
	version = "0.1.6",
	description = "Bramble MegaFlash $C0C0-$C0C3 TCP bridge",
	license = "BSD-3-Clause",
	author = { name = "eositis" }
}

local plugin = exports

-- OPEN_FLAG_READ | OPEN_FLAG_WRITE  (do NOT include CREATE=4)
local OPEN_RW = 3

local function getenv_port()
	local p = os.getenv("BRAMBLE_A2BUS_PORT")
	if p and tonumber(p) then
		return tonumber(p)
	end
	return 19765
end

local function logf(fmt, ...)
	emu.print_info(string.format("megaflash_bridge: " .. fmt, ...))
end

local function load_iic_rom()
	local path = os.getenv("BRAMBLE_IIC_BIN")
	if not path or path == "" then
		return
	end

	local data = nil
	local f = io.open(path, "rb")
	if f then
		data = f:read("*a")
		f:close()
	else
		local ef = emu.file("", 1)
		if ef:open(path) then
			emu.print_error(string.format("megaflash_bridge: cannot open IIC_BIN %s", path))
			return
		end
		data = ef:read(0x8000)
		ef:close()
	end

	if not data or #data < 0x8000 then
		emu.print_error(string.format("megaflash_bridge: IIC_BIN too small (%s)", path))
		return
	end

	local region = manager.machine.memory.regions[":maincpu"]
	if not region then
		return
	end

	for i = 0, 0x7fff do
		region:write_u8(i, data:byte(i + 1))
	end

	logf("overlaid %s (:maincpu[0x408]=0x%02X)", path, region:read_u8(0x408))
end

local function try_open_bridge()
	local port = getenv_port()
	local sock = emu.file("", OPEN_RW)
	local path = string.format("socket.127.0.0.1:%d", port)
	local err = sock:open(path)
	if err then
		return nil, tostring(err)
	end
	return sock, nil
end

local function rpc(sock, req)
	if not sock then
		return nil
	end
	sock:write(req)
	local deadline = os.clock() + 2.0
	local buf = ""
	while #buf < 2 do
		local chunk = sock:read(2 - #buf)
		if chunk and #chunk > 0 then
			buf = buf .. chunk
		elseif os.clock() > deadline then
			return nil
		end
	end
	if buf:byte(1) ~= 0 then
		return nil
	end
	return buf:byte(2)
end

function plugin.startplugin()
	local sock = nil
	local taps = {}
	local tap_count = 0
	local connect_tries = 0
	local taps_ready = false

	local function ensure_sock()
		if sock then
			return true
		end
		connect_tries = connect_tries + 1
		local s, err = try_open_bridge()
		if not s then
			if connect_tries <= 5 or (connect_tries % 60) == 0 then
				emu.print_error(string.format(
					"megaflash_bridge: connect 127.0.0.1:%d failed (%s) try #%d",
					getenv_port(), err or "?", connect_tries))
			end
			return false
		end
		sock = s
		local pong = rpc(sock, string.char(0x00))
		logf("connected port=%d PING=0x%02X", getenv_port(), pong or -1)
		return true
	end

	local function nibble_from_offset(offset)
		local addr = offset
		if addr < 0x100 then
			addr = 0xc0c0 + (addr & 0x3)
		end
		return addr & 0x3, addr
	end

	local function install_taps()
		local cpu = manager.machine.devices[":maincpu"]
		if not cpu then
			return false
		end
		local space = cpu.spaces["program"]
		if not space then
			return false
		end

		load_iic_rom()
		ensure_sock()

		if taps.read then
			taps.read:remove()
			taps.read = nil
		end
		if taps.write then
			taps.write:remove()
			taps.write = nil
		end
		if taps.nsc then
			taps.nsc:remove()
			taps.nsc = nil
		end

		-- apple2c4 always has a built-in DS1216E "no-slot clock". When its
		-- pattern matches, /CEO substitutes clock bits for Cnxx ROM reads and
		-- ProDOS may claim that clock instead of MegaFlash. Mute by restoring
		-- maincpu ROM bytes whenever the read data diverges from either bank.
		local region = manager.machine.memory.regions[":maincpu"]
		if region then
			taps.nsc = space:install_read_tap(0xc100, 0xcfff, "megaflash_nsc_mute",
				function(offset, data, mask)
					local addr = offset
					if addr < 0x1000 then
						addr = 0xc000 + (addr & 0xfff)
					end
					local off = addr - 0xc000
					if off < 0 or off > 0xfff then
						return data
					end
					local b0 = region:read_u8(off)
					local b1 = region:read_u8(off + 0x4000)
					if data == b0 or data == b1 then
						return data
					end
					return b0
				end)
			logf("NSC muted on $C100-$CFFF (use MegaFlash clock)")
		end

		taps.read = space:install_read_tap(0xc0c0, 0xc0c3, "megaflash_r",
			function(offset, data, mask)
				if not ensure_sock() then
					return data
				end
				local nibble, addr = nibble_from_offset(offset)
				local v = rpc(sock, string.char(0x02, nibble))
				tap_count = tap_count + 1
				if tap_count <= 40 then
					logf("RD $%04X -> %s (#%d)", addr, v and string.format("0x%02X", v) or "nil", tap_count)
				end
				if v then
					return v
				end
				sock:close()
				sock = nil
				return data
			end)

		taps.write = space:install_write_tap(0xc0c0, 0xc0c3, "megaflash_w",
			function(offset, data, mask)
				if not ensure_sock() then
					return
				end
				local nibble, addr = nibble_from_offset(offset)
				local byte = data & 0xff
				local ok = rpc(sock, string.char(0x03, nibble, byte))
				tap_count = tap_count + 1
				if tap_count <= 40 then
					logf("WR $%04X <- 0x%02X (#%d)", addr, byte, tap_count)
				end
				if not ok then
					sock:close()
					sock = nil
				end
			end)

		taps_ready = true
		logf("taps installed sock=%s", sock and "up" or "down (will retry)")
		return true
	end

	emu.add_machine_reset_notifier(function()
		taps_ready = false
		install_taps()
	end)

	-- Reset may already have fired before the plugin registered; also reconnect.
	emu.register_periodic(function()
		if not taps_ready then
			install_taps()
		elseif not sock then
			ensure_sock()
		end
	end)

	emu.add_machine_stop_notifier(function()
		logf("stop after %d taps", tap_count)
		taps_ready = false
		taps = {}
		if sock then
			sock:close()
			sock = nil
		end
	end)
end

return exports
