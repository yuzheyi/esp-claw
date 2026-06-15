-- FOC Motor Monitor - Display FOC status on ST7789 LCD
local bm = require("board_manager")
local lvgl = require("lvgl")
local cap = require("capability")
local delay = require("delay")

-- Get LCD handles from board manager
local panel, io, w, h, iface = bm.get_display_lcd_params("display_lcd")
if not panel then
    print("ERROR: display_lcd not found: " .. (io or "unknown"))
    return
end

-- Initialize LVGL with the LCD
lvgl.init(panel, io, w, h, iface)
print(string.format("LVGL init: %dx%d iface=%d", w, h, iface))

-- Create a new screen
local scr = lvgl.create_screen()
scr:set_style_bg_color("#111111", 0)
scr:load()

-- Title
local title = lvgl.label(scr)
title:set_text("FOC Motor Monitor")
title:set_style_text_color("#00FF00", 0)
title:set_pos(10, 5)

-- Status lines
local lines = {}
local labels = {"State:", "Mode:", "Theta:", "Iu (mA):", "Iv (mA):", "Tgt Id:", "Tgt Iq:"}
for i, lbl in ipairs(labels) do
    local line = lvgl.label(scr)
    line:set_text(lbl .. " ---")
    line:set_style_text_color("#CCCCCC", 0)
    line:set_pos(10, 30 + (i - 1) * 22)
    lines[i] = line
end

-- Loop: update display every 300ms
while true do
    local ok, result = pcall(function()
        return cap.call("foc_motor_status", {})
    end)

    if ok and result then
        local state_str = string.match(result, "running=(%d)") or "?"
        local mode_str = string.match(result, "mode=([%w-]+)") or "?"
        local theta_str = string.match(result, "theta=([%d%.%-]+)") or "0"
        local iu_str = string.match(result, "Iu=([%d%.%-]+)") or "0"
        local iv_str = string.match(result, "Iv=([%d%.%-]+)") or "0"
        local id_str = string.match(result, "target_Id=([%d%.%-]+)") or "0"
        local iq_str = string.match(result, "target_Iq=([%d%.%-]+)") or "0"

        local state_text = (state_str == "1") and "RUNNING" or "STOPPED"

        lines[1]:set_text("State: " .. state_text)
        lines[2]:set_text("Mode: " .. mode_str)
        lines[3]:set_text("Theta: " .. theta_str .. " rad")
        lines[4]:set_text("Iu: " .. iu_str .. " mA")
        lines[5]:set_text("Iv: " .. iv_str .. " mA")
        lines[6]:set_text("Tgt Id: " .. id_str)
        lines[7]:set_text("Tgt Iq: " .. iq_str)
    else
        lines[1]:set_text("State: ERROR")
    end

    delay.ms(300)
end
