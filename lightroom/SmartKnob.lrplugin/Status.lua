-- File > Plug-in Extras > SmartKnob status: shows what the plugin is doing,
-- and starts it if Lightroom didn't run Init.lua by itself.
local LrDialogs = import 'LrDialogs'

local note = ''
if not SmartKnobStarted then
    -- Lightroom's require() runs a script from this plugin's folder
    local ok, err = pcall(require, 'Init')
    note = ok and 'Was not running: started it now.\n' or ('Could not start: ' .. tostring(err) .. '\n')
end

LrDialogs.message('SmartKnob', note .. 'Status: ' .. tostring(SmartKnobStatus))
