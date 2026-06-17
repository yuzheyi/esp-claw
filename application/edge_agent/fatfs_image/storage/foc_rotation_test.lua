-- FOC Motor Monitor - Persistent display of FOC parameters on ST7789
-- Designed for 22N24P motor (12 pole pairs), runs as async persistent task.
-- Usage (async):
--   lua --run-async --path /fatfs/foc_rotation_test.lua --timeout-ms 0
-- Usage (sync, 2 min):
--   lua --run --path /fatfs/foc_rotation_test.lua --timeout-ms 120000
local bm     = require("board_manager")
local cap    = require("capability")
local delay  = require("delay")
local lvgl   = require("lvgl")

-- ── Configuration (overridable via args) ──────────────────────
local args = args or {}
local POLE_PAIRS      = args.pole_pairs or 12      -- 22N24P motor
local PWM_FREQ_HZ     = args.pwm_freq_hz or 20000
local OPENLOOP_SPEED  = args.openloop_speed_rad_s or 20.0  -- rad/s electrical
local TARGET_IQ_MA    = args.target_iq_ma or 0     -- 0 = pure voltage open-loop
local UPDATE_MS       = 150     -- display refresh interval

-- ── LCD Init ─────────────────────────────────────────────────────
local panel, io, w, h, iface = bm.get_display_lcd_params("display_lcd")
if not panel then
    print("ERROR: display_lcd not found: " .. (io or "unknown"))
    return
end
lvgl.init(panel, io, w, h, iface)
print(string.format("LCD ready: %dx%d", w, h))

-- ── LVGL UI ──────────────────────────────────────────────────────
local scr = lvgl.create_screen()
scr:set_style({bg_color = "#0a0e1a"})
scr:load()

-- Title label
local title = lvgl.label(scr, {text = "FOC  22N24P  P=12"})
title:set_pos(8, 4)
title:set_style({text_color = "#00e5ff"})

-- Separator
local sep = lvgl.label(scr, {text = "________________________________"})
sep:set_pos(8, 22)
sep:set_style({text_color = "#333344"})

-- Parameter labels
local Y_START   = 40
local Y_STEP    = 20
local COL_NAME  = 8
local COL_VAL   = 125

local param_defs = {
    { name = "Status",      color = "#44ff88" },
    { name = "Mode",        color = "#ffcc44" },
    { name = "Elec Speed",  color = "#cccccc" },
    { name = "RPM",         color = "#ff66aa" },
    { name = "Theta",       color = "#cccccc" },
    { name = "Iu",          color = "#ff8844" },
    { name = "Iv",          color = "#ff8844" },
    { name = "Iw(est)",     color = "#ff8844" },
    { name = "Target Id",   color = "#88aaff" },
    { name = "Target Iq",   color = "#88aaff" },
}

local name_labels = {}
local val_labels  = {}

for i, def in ipairs(param_defs) do
    local y = Y_START + (i - 1) * Y_STEP

    local nl = lvgl.label(scr, {text = def.name .. ":"})
    nl:set_pos(COL_NAME, y)
    nl:set_style({text_color = "#888899"})
    name_labels[i] = nl

    local vl = lvgl.label(scr, {text = "---"})
    vl:set_pos(COL_VAL, y)
    vl:set_style({text_color = def.color})
    val_labels[i] = vl
end

-- Arc indicator for rotation angle
local arc = lvgl.arc(scr)
arc:set_pos(w - 80, Y_START)
arc:set_size(70, 70)
arc:set_range(0, 360)
arc:set_style({line_color = "#00e5ff", arc_width = 6})
arc:set_value(0)

local arc_lbl = lvgl.label(scr, {text = "Angle"})
arc_lbl:set_pos(w - 72, Y_START + 74)
arc_lbl:set_style({text_color = "#666688"})

-- Helper
local function set_val(idx, text)
    val_labels[idx]:set_text(text)
end

-- ── FOC Motor Init ──────────────────────────────────────────────
print("Initializing FOC motor...")
local ok, out, err = cap.call("foc_motor_init", {
    pole_pairs      = POLE_PAIRS,
    pwm_freq_hz     = PWM_FREQ_HZ,
    shunt_resistance = 0.001,
})
print("init:", ok, out, err)
if not ok then
    set_val(1, "INIT FAIL")
    delay.delay_ms(5000)
    return
end

-- ── FOC Motor Start ─────────────────────────────────────────────
print("Starting motor (open-loop)...")
ok, out, err = cap.call("foc_motor_start", {
    target_iq_ma         = TARGET_IQ_MA,
    target_id_ma         = 0,
    openloop_speed_rad_s = OPENLOOP_SPEED,
    closed_loop          = false,
})
print("start:", ok, out, err)
if not ok then
    set_val(1, "START FAIL")
    delay.delay_ms(5000)
    return
end

-- ── Main Display Loop (fixed speed, no ramp) ─────────────────
print("Entering display loop (fixed speed)...")
local cycle = 0

while true do
    cycle = cycle + 1

    -- Read status (cap.call returns: ok, output, error)
    local ok2, result, err2 = cap.call("foc_motor_status", {})

    if ok2 and result then
        local running = string.match(result, "running=(%d)") or "?"
        local mode    = string.match(result, "mode=([%w-]+)") or "?"
        local theta   = tonumber(string.match(result, "theta=([%d%.%-]+)") or "0") or 0
        local iu      = tonumber(string.match(result, "Iu=([%d%.%-]+)") or "0") or 0
        local iv      = tonumber(string.match(result, "Iv=([%d%.%-]+)") or "0") or 0
        local tid     = tonumber(string.match(result, "target_Id=([%d%.%-]+)") or "0") or 0
        local tiq     = tonumber(string.match(result, "target_Iq=([%d%.%-]+)") or "0") or 0

        local iw = -(iu + iv)
        local theta_deg = theta * 180.0 / 3.14159265
        -- Get live speed from status (fallback to config)
        local live_speed = tonumber(string.match(result, "speed=([%d%.%-]+)") or "") or OPENLOOP_SPEED
        -- Mechanical RPM = elec_speed / (2*pi*pole_pairs) * 60
        local mech_rpm  = live_speed * 60.0 / (2.0 * 3.14159265 * POLE_PAIRS)

        set_val(1, (running == "1") and "RUNNING" or "STOPPED")
        set_val(2, mode)
        set_val(3, string.format("%.1f rad/s", live_speed))
        set_val(4, string.format("%.1f", mech_rpm))
        set_val(5, string.format("%.1f deg", theta_deg))
        set_val(6, string.format("%.1f mA", iu))
        set_val(7, string.format("%.1f mA", iv))
        set_val(8, string.format("%.1f mA", iw))
        set_val(9, string.format("%.0f mA", tid))
        set_val(10, string.format("%.0f mA", tiq))

        -- Update arc (electrical angle 0-360)
        local arc_val = math.floor(theta_deg) % 360
        arc:set_value(arc_val)
    else
        set_val(1, "READ ERR")
        if err2 then print("status err:", err2) end
    end

    delay.delay_ms(UPDATE_MS)
end
