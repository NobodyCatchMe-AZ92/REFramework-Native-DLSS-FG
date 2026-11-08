-- Native DLSS + Frame Generation: settings menu and overlay for its plugin (reframework/plugins/refw_framegen.dll).
--
-- The plugin reads reframework/data/refw_framegen.txt ("key=value" lines) and reloads it when it changes; this menu
-- writes that file. The plugin writes reframework/data/refw_framegen_status.txt once a second, shown at the bottom.
-- Only REFramework's basic Lua API is used (imgui, fs, re.on_draw_ui), so this works across REFramework versions.

-- The mod's name and version (menu title, overlay); package.sh checks the version against the plugin's and the package's.
local MOD_NAME = "Native DLSS + Frame Generation"
local MOD_SHORT = "Native DLSS + FG"
local MOD_VERSION = "1.0"

local SETTINGS = "refw_framegen.txt"
local STATUS = "refw_framegen_status.txt"

local MODES = { "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance" }
local MULTIPLIERS = { "2x", "3x", "4x", "5x", "6x" }
local PRESET_NAMES = { "Default", "J", "K", "L", "M" }
local PRESET_VALUES = { 0, 10, 11, 12, 13 }

-- The settings file as lines (comments and unknown keys are kept when it is written back).
local lines = {}
local values = {}

local function load_settings()
    lines = {}
    values = {}
    local text = fs.read(SETTINGS) or ""

    for line in text:gmatch("[^\r\n]+") do
        table.insert(lines, line)
        local key, value = line:match("^%s*([%w_]+)%s*=%s*(.-)%s*$")

        if key ~= nil then
            values[key] = value
        end
    end
end

local function save_settings()
    local written = {}
    local out = {}

    for _, line in ipairs(lines) do
        local key = line:match("^%s*([%w_]+)%s*=")

        if key ~= nil and values[key] ~= nil then
            table.insert(out, key .. "=" .. values[key])
            written[key] = true
        else
            table.insert(out, line)
        end
    end

    for key, value in pairs(values) do
        if not written[key] then
            table.insert(out, key .. "=" .. value)
        end
    end

    fs.write(SETTINGS, table.concat(out, "\n") .. "\n")
    load_settings()
end

local function get_number(key, default)
    return tonumber(values[key]) or default
end

local function get_bool(key, default)
    local value = values[key]

    if value == nil then
        return default
    end

    return value ~= "0"
end

local function set(key, value)
    if type(value) == "boolean" then
        value = value and 1 or 0
    end

    -- The file as it is now (it can be edited by hand while the game runs): only this key changes, nothing else is lost.
    load_settings()
    values[key] = tostring(value)
    save_settings()
end

-- The plugin's status, read at most twice a second while the menu is open.
local status = {}
local status_time = -1

local function read_status()
    local now = os.clock()

    if now - status_time < 0.5 then
        return
    end

    status_time = now
    status = {}
    local text = fs.read(STATUS) or ""

    for key, value in text:gmatch("([%w_]+)=([^\r\n]*)") do
        status[key] = value
    end
end

local function status_number(key)
    return tonumber(status[key]) or 0
end

local GREY = 0xFFAAAAAA
local GREEN = 0xFF66DD66
local YELLOW = 0xFF44DDEE
local WHITE = 0xFFFFFFFF

local CORNERS = { "Top left", "Top right", "Bottom left", "Bottom right" }

load_settings()

-- The overlay (like NVIDIA's DLSS indicator): a small frameless window, drawn every frame while it is switched on.
local OVERLAY_FLAGS = 1 + 2 + 4 + 8 + 32 + 64 + 256 + 512 + 4096 + 8192 + 65536 + 131072 -- no title/resize/move/inputs, auto size

local function mode_name(mode)
    return MODES[(mode or 1) + 1] or "?"
end

local function preset_name(preset)
    for i, v in ipairs(PRESET_VALUES) do
        if v == preset then
            return PRESET_NAMES[i]
        end
    end

    return tostring(preset)
end

-- The DLSS preset actually used: NVIDIA's override (NVIDIA app / control panel) wins over ours; "latest" and no preset
-- mean the newest model for the mode (DLSS 310: K for DLAA/Quality/Balanced, M for Performance, L for Ultra Performance).
local LATEST_FOR_MODE = { [0] = "K", [1] = "K", [2] = "K", [3] = "M", [4] = "L" }

local function active_preset(mode)
    local latest = LATEST_FOR_MODE[mode] or "?"
    local forced = status_number("nv_sr_override")

    if forced == 0xFFFFFF then
        return "preset " .. latest .. " (NVIDIA app: latest)"
    elseif forced > 0 and forced <= 26 then
        return "preset " .. string.char(64 + forced) .. " (NVIDIA app)"
    elseif forced > 0 then
        return string.format("preset set by NVIDIA app (code 0x%X)", forced)
    end

    local ours = status_number("set_dlss_preset")

    if ours > 0 then
        return "preset " .. string.char(64 + ours)
    end

    return "preset " .. latest .. " (default)"
end

local function place_overlay(corner)
    local ok, size = pcall(imgui.get_display_size)

    if not ok or size == nil then
        imgui.set_next_window_pos(Vector2f.new(10, 10), 1, Vector2f.new(0, 0)) -- top left without the display size
        return
    end

    local right = corner == 1 or corner == 3
    local bottom = corner == 2 or corner == 3
    local x = right and (size.x - 10) or 10
    local y = bottom and (size.y - 10) or 10
    imgui.set_next_window_pos(Vector2f.new(x, y), 1, Vector2f.new(right and 1 or 0, bottom and 1 or 0))
end

re.on_frame(function()
    if not get_bool("overlay", false) then
        return
    end

    read_status()
    place_overlay(get_number("overlay_corner", 0))
    pcall(imgui.set_next_window_bg_alpha, 0.6)

    if imgui.begin_window("Native DLSS + FG overlay", true, OVERLAY_FLAGS) then
        local game = status_number("fps_game")
        imgui.text_colored(MOD_SHORT .. " " .. MOD_VERSION, WHITE)

        if status["error"] ~= nil then
            imgui.text_colored(status["error"], YELLOW)
        elseif game == 0 and status_number("fps_shown") == 0 then
            imgui.text_colored("plugin not running", YELLOW)
        else
            -- DLSS: version, mode, preset, resolutions.
            if status_number("dlss_running") == 1 then
                local render_w, render_h = status_number("render_width"), status_number("render_height")
                local out_w, out_h = status_number("output_width"), status_number("output_height")
                local version = status["dlss_version"] or "?"
                local preset_text = active_preset(status_number("set_dlss_mode"))
                imgui.text_colored(string.format("DLSS %s  %s  %s", version, mode_name(status["active_dlss_mode"] ~= nil and status_number("active_dlss_mode") or status_number("set_dlss_mode")), preset_text), GREEN)
                imgui.text_colored(string.format("%dx%d -> %dx%d (%.1f%%%%)", render_w, render_h, out_w, out_h,
                    out_w > 0 and 100 * render_w / out_w or 0), WHITE)

                local lod_scale = status_number("lod_scale")

                if lod_scale > 0 and lod_scale ~= 1 then
                    imgui.text_colored(string.format("Texture LOD bias %.2f", math.log(lod_scale) / math.log(2)), WHITE)
                end
            else
                imgui.text_colored("DLSS off", GREY)
            end

            -- Frame generation.
            local shown = status_number("fps_shown")

            if status_number("set_enabled") == 1 and status_number("fg_frames_presented") > 1 then
                imgui.text_colored(string.format("Frame gen %dx (DLSS-G %s)%s", status_number("fg_frames_presented"),
                    status["dlssg_version"] or "?", status_number("hud_fix_active") == 1 and "  HUD fix" or ""), GREEN)
            else
                imgui.text_colored("Frame gen off", GREY)
            end

            imgui.text_colored(string.format("%.0f fps rendered, %.0f shown", game, shown), WHITE)

            -- A pd-upscaler REFramework build: its own DLSS (PureDark's PDPerfPlugin.dll) kept off by this plugin all session.
            if status["pd_upscaler"] == "1" then
                imgui.text_colored("REFramework's default upscaling has been successfully replaced", WHITE)
                imgui.text_colored("by the standalone DLSS plugin's better implementation.", WHITE)
            end
        end
    end

    imgui.end_window()
end)

re.on_draw_ui(function()
    if not imgui.tree_node(MOD_NAME .. " " .. MOD_VERSION) then
        return
    end

    read_status()

    if status["error"] ~= nil then
        imgui.text_colored("Problem: " .. status["error"], YELLOW)
    elseif status_number("fps_game") == 0 and status_number("fps_shown") == 0 then
        imgui.text_colored("The plugin is not running (reframework/plugins/refw_framegen.dll).", YELLOW)
    end

    -- A pd-upscaler REFramework build (status pd_upscaler=1): its own upscaler is kept off at startup, this plugin's DLSS is used.
    if status["pd_upscaler"] == "1" then
        imgui.text_colored("REFramework's own upscaler (pd-upscaler build): kept off, this plugin's DLSS is used instead.", GREY)
        imgui.text_colored("  (override_pd_upscaler=0 in refw_framegen.txt hands it back; this plugin is then off. Restart needed.)", GREY)
    end

    local changed, value

    -- Frame generation.
    imgui.text("Frame generation (DLSS-G)")

    changed, value = imgui.checkbox("Frame generation", get_bool("enabled", true))
    if changed then set("enabled", value) end

    local generated = math.max(1, math.min(5, get_number("generated", 1)))
    changed, value = imgui.combo("Multiplier", generated, MULTIPLIERS)
    if changed then set("generated", value) end

    if status_number("fg_max_generated") > 0 then
        imgui.text_colored(string.format("  Your GPU/driver allows up to %dx (higher is stepped down).", status_number("fg_max_generated") + 1), GREY)
    end

    imgui.separator()

    -- DLSS super resolution.
    imgui.text("DLSS super resolution")

    changed, value = imgui.checkbox("DLSS", get_bool("dlss", true))
    if changed then set("dlss", value) end

    local mode = math.max(0, math.min(4, get_number("dlss_mode", 1)))
    changed, value = imgui.combo("Mode", mode + 1, MODES)
    if changed then set("dlss_mode", value - 1) end

    -- A preset forced in the NVIDIA app always wins: then no preset choice here, just which one is in use.
    local forced = status_number("nv_sr_override")

    if forced > 0 then
        local letter = "?"

        if forced == 0xFFFFFF then
            letter = (LATEST_FOR_MODE[mode] or "?") .. " (Latest)"
        elseif forced <= 26 then
            letter = string.char(64 + forced)
        end

        imgui.text_colored("If you want to manually select a DLSS Preset in this menu, you need to disable the Nvidia App", GREY)
        imgui.text_colored("DLSS override first. You currently have a DLSS Override active in your Nvidia App settings.", GREY)
        imgui.text_colored("Current Nvidia forced Preset: " .. letter, GREEN)
    else
        local preset = get_number("dlss_preset", 0)
        local preset_index = 1

        for i, v in ipairs(PRESET_VALUES) do
            if v == preset then
                preset_index = i
            end
        end

        changed, value = imgui.combo("Preset", preset_index, PRESET_NAMES)
        if changed then set("dlss_preset", PRESET_VALUES[value]) end
    end

    -- FidelityFX CAS sharpening on DLSS's output (off by default).
    changed, value = imgui.checkbox("Enable FidelityFX CAS Sharpening", get_bool("dlss_cas", true))
    if changed then set("dlss_cas", value) end
    imgui.text_colored("  Use this plugin's implementation of CAS Sharpening to sharpen the DLSS image. Leave unticked if you prefer to use", GREY)
    imgui.text_colored("  your own sharpening filter (e.g. via Reshade)", GREY)

    if get_bool("dlss_cas", true) then
        changed, value = imgui.slider_float("Sharpening", get_number("dlss_cas_strength", 0.5), 0.0, 1.0, "%.2f")
        if changed then set("dlss_cas_strength", string.format("%.2f", value)) end
        imgui.same_line()
        if imgui.button("Default##cas") then set("dlss_cas_strength", "0.50") end
    end

    imgui.separator()

    changed, value = imgui.checkbox("Show overlay (DLSS / frame generation info on screen)", get_bool("overlay", false))
    if changed then set("overlay", value) end

    changed, value = imgui.combo("Overlay position", get_number("overlay_corner", 0) + 1, CORNERS)
    if changed then set("overlay_corner", value - 1) end

    if imgui.tree_node("Advanced") then
        local auto_lod = get_bool("dlss_lod_bias", true)
        changed, value = imgui.checkbox("Automatic texture LOD bias", auto_lod)
        if changed then
            set("dlss_lod_bias", value)
            auto_lod = value
        end

        -- The bias in use now (status lod_scale: the scale on the textures' UV derivatives; 1 = none).
        local lod_scale = status_number("lod_scale")

        if status_number("dlss_running") == 1 and lod_scale > 0 and lod_scale ~= 1 then
            imgui.text_colored(string.format("  Current texture LOD bias: %.2f mip levels", math.log(lod_scale) / math.log(2)), GREEN)
        else
            imgui.text_colored("  Current texture LOD bias: none (DLSS off or automatic bias off)", GREY)
        end

        -- The offset is part of the automatic bias (log2(render / output) + offset): without it, it does nothing (greyed out).
        local can_disable = imgui.begin_disabled ~= nil and imgui.end_disabled ~= nil

        if auto_lod or can_disable then
            if can_disable then imgui.begin_disabled(not auto_lod) end
            changed, value = imgui.slider_float("LOD bias offset", get_number("dlss_lod_bias_offset", -1.0), -3.0, 1.0, "%.2f mip levels")
            if changed and auto_lod then set("dlss_lod_bias_offset", string.format("%.2f", value)) end
            if can_disable then imgui.end_disabled() end
            imgui.text_colored("  Added to log2(render / output). -1 is DLSS's recommendation; lower is sharper but can shimmer.", GREY)
        end

        -- The engine's forced sharpening and TAA while DLSS is on (setting dlss_sharpening_off: 1 = off unless another mod
        -- does it, 0 = kept). The game's TAA is always replaced by DLSS while DLSS is on.
        changed, value = imgui.checkbox("Disable the game's native forced Sharpening+TAA", get_number("dlss_sharpening_off", 1) ~= 0)
        if changed then set("dlss_sharpening_off", value and 1 or 0) end
        read_status()
        local sharpening = status["sharpening"] or ""
        local mod = status["sharpening_mod"] or ""
        local kind = status["sharpening_mod_kind"] or ""

        if sharpening == "plugin" then
            imgui.text_colored("  Sharpening+TAA disabled by this mod", GREEN)
        elseif sharpening == "mod" then
            imgui.text_colored(string.format("  Sharpening+TAA disabled by existing %s %s", kind, mod), GREEN)
            imgui.text_colored("  (this mod leaves it to that one; both together are fine)", GREY)
        elseif sharpening == "in_upscale" then
            imgui.text_colored("  Sharpening+TAA disabled while DLSS is on: in this game they are part of its own", GREEN)
            imgui.text_colored("  upscale, which DLSS replaces.", GREEN)

            if mod ~= "" then
                imgui.text_colored(string.format("  Without DLSS: sharpening disabled by existing %s %s", kind, mod), GREY)
            end
        elseif sharpening == "kept_mod" then
            imgui.text_colored(string.format("  This mod leaves Sharpening+TAA alone, but existing %s %s still disables them", kind, mod), GREEN)
        elseif sharpening == "kept" then
            imgui.text_colored("  Sharpening kept on (the game's own). TAA is still replaced by DLSS while DLSS is on.", YELLOW)
        elseif sharpening == "dlss_off" then
            imgui.text_colored("  Applies while DLSS is on (DLSS is off: the game's own settings apply)", GREY)
        elseif sharpening == "unavailable" then
            imgui.text_colored("  Unavailable: this game version's sharpening code wasn't recognised (the game's own applies)", YELLOW)
        end

        -- RE7's videotape sections (the plugin reports videotape_mode=1 for its profile).
        if status["videotape_mode"] == "1" then
            changed, value = imgui.checkbox("Disable DLSS during Videotape Sections (RE7 only)", get_bool("dlss_tape_off", false))
            if changed then set("dlss_tape_off", value) end
            imgui.text_colored("  Ticked: videotape sections look exactly as in the unmodded game (native resolution, the game's", GREY)
            imgui.text_colored("  own TAA). Unticked: DLSS up to the tape's own lower resolution, then scaled like the game does.", GREY)
        end

        -- RE2's item examine view (the plugin reports examine_off=1 for its profile): DLAA there (on by default).
        if status["examine_off"] == "1" then
            changed, value = imgui.checkbox("Force DLAA while examining items (fix blurry rotation - RE2, RE3)", get_bool("dlss_examine_dlaa", true))
            if changed then set("dlss_examine_dlaa", value) end
            imgui.text_colored("  A rotating item is softer at the lower DLSS modes. Ticked: DLAA (native resolution) while an item", GREY)
            imgui.text_colored("  is examined. The switch in and out can show a brief dark frame.", GREY)
        end

        -- The plugin's depth of field (RE7: status own_dof=1): its strength, 100% = the game's lens as is.
        if status["own_dof"] == "1" then
            changed, value = imgui.slider_float("Depth of field strength", get_number("dlss_dof_scale", 1.0) * 100.0, 0.0, 200.0, "%.0f%%")
            if changed then set("dlss_dof_scale", string.format("%.2f", value / 100.0)) end
            changed, value = imgui.slider_float("Depth of field max blur", get_number("dlss_dof_max_scale", 1.0) * 100.0, 10.0, 500.0, "%.0f%%")
            if changed then set("dlss_dof_max_scale", string.format("%.2f", value / 100.0)) end
            imgui.text_colored("  100% matches the game's own depth of field (the default).", GREY)
        end

        -- RE8 (status own_bloom=1): the plugin's bloom after DLSS (testing): on/off (off = the game's own, through DLSS), mix,
        -- strength and threshold, to match the game's look by eye.
        if status["own_bloom"] == "1" then
            changed, value = imgui.checkbox("Bloom after DLSS (no bloom flicker; unticked: the game's own bloom)", get_bool("dlss_own_bloom", true))
            if changed then set("dlss_own_bloom", value) end
            changed, value = imgui.checkbox("  Bloom: additive mix", get_bool("dlss_own_bloom_add", true))
            if changed then set("dlss_own_bloom_add", value) end
            changed, value = imgui.slider_float("  Bloom strength", get_number("dlss_own_bloom_strength", 1.0), 0.0, 3.0, "x%.2f")
            if changed then set("dlss_own_bloom_strength", string.format("%.2f", value)) end
            changed, value = imgui.slider_float("  Bloom threshold", get_number("dlss_own_bloom_threshold", -1.0), -1.0, 1.0, "%.3f")
            if changed then set("dlss_own_bloom_threshold", string.format("%.3f", value)) end
            changed, value = imgui.slider_float("  Bloom inner glow", get_number("dlss_own_bloom_inner", -1.0), -1.0, 20.0, "%.1f")
            if changed then set("dlss_own_bloom_inner", string.format("%.2f", value)) end
            imgui.text_colored("  Defaults match the game's own bloom: strength x1.00, threshold and inner glow -1 (the game's / calibrated).", GREY)
        end

        -- RE2 only (status re2_dither=1): the [EXPERIMENTAL] hair/shadow dither fix, off by default. On = the 4x4 shift loop
        -- (setting dlss_dither_test 2; the other values are research modes, see re2_dither.hpp).
        if status["re2_dither"] == "1" then
            changed, value = imgui.checkbox("[EXPERIMENTAL] Hair/shadow dither fix (RE2)", get_number("dlss_dither_test", 0) ~= 0)
            if changed then set("dlss_dither_test", value and 2 or 0) end
            imgui.text_colored("  An experimental attempt at cleaning up the dithering pattern in hair and shadows. It produces improvements, but they are only very minor.", GREY)
        end

        changed, value = imgui.checkbox("Keep bloom out of DLSS (experimental, artifacts around lights)", get_bool("dlss_bloom", false))
        if changed then set("dlss_bloom", value) end

        -- Testing only: keys that switch features off live (left on by accident they look like bugs, e.g. F6's ghosting).
        changed, value = imgui.checkbox("Diagnostic hotkeys (for testing)", get_bool("debug_keys", false))
        if changed then set("debug_keys", value) end
        imgui.text_colored("  F5 HUD fix, F6 frame generation inputs, F7 DLSS, F8 LOD bias: each switches its feature off/on", GREY)
        imgui.text_colored("  (game window in front). Unticking puts everything back to normal.", GREY)

        imgui.tree_pop()
    end

    imgui.separator()

    -- Status.
    local game = status_number("fps_game")
    local shown = status_number("fps_shown")

    if game > 0 then
        imgui.text_colored(string.format("%.0f fps rendered, %.0f shown (x%.2f)", game, shown, shown / game), GREEN)
    end

    if status_number("dlss_running") == 1 then
        imgui.text_colored(string.format("DLSS: %dx%d -> %dx%d, texture LOD scale %.3f", status_number("render_width"),
            status_number("render_height"), status_number("output_width"), status_number("output_height"), status_number("lod_scale")), GREEN)
    elseif get_bool("dlss", true) then
        if status_number("dlss_available") == 0 and game > 0 then
            imgui.text_colored("DLSS is not available (GPU/driver, or nvngx_dlss.dll / sl.dlss.dll missing).", YELLOW)
        else
            imgui.text_colored("DLSS: waiting for a 3D scene.", GREY)
        end
    end

    imgui.tree_pop()
end)
