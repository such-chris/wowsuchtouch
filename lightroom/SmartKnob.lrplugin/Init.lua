-- SmartKnob: lets the knob set Develop sliders directly and follow the open tool.
--
--   knob --USB--> KnobLink (Mac) --local socket--> this plugin --> Lightroom
--
-- Lightroom sockets only go one way, so there are two:
--   RECEIVE_PORT: KnobLink -> Lightroom   e.g. {"t":"turn","param":"Exposure2012","d":3}
--   SEND_PORT:    Lightroom -> KnobLink   e.g. {"t":"val","param":"Exposure2012","v":0.35,"txt":"+0.35"}
--                                              {"t":"tool","tool":"masking"}
--                                              {"t":"module","module":"develop"}
-- Messages are one line of JSON each. Log: ~/Documents/LrClassicLogs/SmartKnob.log

local LrTasks = import 'LrTasks'
local LrFunctionContext = import 'LrFunctionContext'
local LrSocket = import 'LrSocket'
local LrDevelopController = import 'LrDevelopController'
local LrApplicationView = import 'LrApplicationView'
local LrApplication = import 'LrApplication'
local LrLogger = import 'LrLogger'
local LrDialogs = import 'LrDialogs'

local log = LrLogger('SmartKnob')
log:enable('logfile')
log:trace('Init.lua loading')

local RECEIVE_PORT = 58810
local SEND_PORT = 58811
local TOOL_CHECK_SECONDS = 0.05   -- also how often queued value replies go out

if SmartKnobStarted then
    log:trace('already running')
    return
end
SmartKnobStarted = true
SmartKnobRunning = true
SmartKnobStatus = 'starting'

-- ---------------- Tiny JSON (flat objects of strings and numbers only) ----------------

local function decode(line)
    local message = {}
    for key, value in string.gmatch(line, '"([%w_]+)"%s*:%s*([^,}]+)') do
        value = value:gsub('^%s+', ''):gsub('%s+$', '')
        if value:sub(1, 1) == '"' then
            message[key] = value:sub(2, -2)
        else
            message[key] = tonumber(value)
        end
    end
    return message
end

local function encode(message)
    local parts = {}
    for key, value in pairs(message) do
        if type(value) == 'number' then
            parts[#parts + 1] = string.format('"%s":%s', key, string.format('%.6g', value))
        else
            parts[#parts + 1] = string.format('"%s":"%s"', key, tostring(value))
        end
    end
    return '{' .. table.concat(parts, ',') .. '}'
end

-- ---------------- Develop sliders ----------------

local function inDevelop()
    return LrApplicationView.getCurrentModuleName() == 'develop'
end

-- How a value reads on the knob: Exposure "+0.35", Contrast "+12", Temp "5500"
local function valueFormat(param)
    local low, high = LrDevelopController.getRange(param)
    if low and high and (high - low) <= 20 then
        return '%+.2f'
    elseif low and low < 0 then
        return '%+d'
    else
        return '%d'
    end
end

local function valueText(param, value)
    local fmt = valueFormat(param)
    if fmt == '%+.2f' then return string.format(fmt, value) end
    return string.format(fmt, math.floor(value + 0.5))
end

-- ---------------- Main loop ----------------

LrTasks.startAsyncTask(function()
    LrFunctionContext.callWithContext('SmartKnob', function(context)
        -- Any error in here: say so on screen and in the log, instead of failing silently
        context:addFailureHandler(function(status, message)
            log:trace('stopped with an error: ' .. tostring(message))
            SmartKnobStatus = 'error: ' .. tostring(message)
            SmartKnobStarted = false
            LrDialogs.message('SmartKnob plugin stopped', tostring(message), 'critical')
        end)
        local sender = nil
        local sender_connected = false
        local current_param = nil   -- the slider the knob is on (from its last message)
        local current_step = nil
        local last_tool = nil
        local last_module = nil
        local last_meta = nil

        local function send(message)
            if sender and sender_connected then
                local ok, err = pcall(function() sender:send(encode(message) .. '\n') end)
                if not ok then log:trace('send failed: ' .. tostring(err)) end
            end
        end

        local last_set_time = 0   -- when we last set a value ourselves (to skip the echo)
        local last_seq = 0        -- the knob's latest click number
        local pending_reply = false
        local pending_full = false   -- the knob opened a slider while Lightroom wasn't in Develop yet
        local last_reply_time = 0

        -- full = name the slider and include its range (on opening it); otherwise just the value
        local function reportValue(param, full)
            if not param or not inDevelop() then return end
            local value = LrDevelopController.getValue(param)
            if type(value) ~= 'number' then return end
            if full then
                local low, high = LrDevelopController.getRange(param)
                send({ t = 'val', param = param, v = value, txt = valueText(param, value),
                       lo = low or 0, hi = high or 0, fmt = valueFormat(param) })
            else
                send({ t = 'val', v = value, s = last_seq })
            end
        end

        -- While the knob turns, answer at most 10 times a second (each answer is a slow
        -- write to the knob); the final answer carries the latest click number
        local function replySoon()
            pending_reply = true
        end

        local function nudge(param, steps, step)
            local low, high = LrDevelopController.getRange(param)
            if not low then return end
            step = step or ((high - low) / 200)   -- 200 detents end to end
            local value = LrDevelopController.getValue(param) + steps * step
            value = math.max(low, math.min(high, value))
            last_set_time = os.clock()
            LrDevelopController.setValue(param, value)
            replySoon()
        end

        local function handle(message)
            if message.t == 'turn' and message.param then
                -- Opening a slider (d = 0): remember it and send value + range
                current_param = message.param
                current_step = message.step
                if not inDevelop() then
                    pending_full = true   -- answer as soon as Develop is showing
                    return
                end
                if (message.d or 0) ~= 0 then
                    nudge(message.param, message.d, message.step)
                end
                reportValue(message.param, true)
            elseif message.t == 'n' and current_param then
                -- A click on the slider the knob is on
                last_seq = message.s or 0
                if not inDevelop() then return end
                nudge(current_param, message.d or 0, current_step)
            elseif message.t == 'act' and message.a and message.a:sub(1, 6) == 'label:' then
                -- Color label (Purple has no shortcut). Like Lightroom's keys it toggles:
                -- if every selected photo already has it, it's removed.
                local label = message.a:sub(7)
                LrTasks.startAsyncTask(function()
                    local catalog = LrApplication.activeCatalog()
                    local photos = catalog:getTargetPhotos()
                    local all_have = #photos > 0
                    for _, photo in ipairs(photos) do
                        if (photo:getRawMetadata('colorNameForLabel') or '') ~= label:lower() then
                            all_have = false
                        end
                    end
                    catalog:withWriteAccessDo('SmartKnob color label', function()
                        for _, photo in ipairs(photos) do
                            photo:setRawMetadata('label', all_have and '' or label)
                        end
                    end)
                end)
            elseif message.t == 'act' and message.a == 'reset' and message.param then
                if not inDevelop() then return end
                last_set_time = os.clock()
                LrDevelopController.resetToDefault(message.param)
                replySoon()
            end
        end

        -- Lightroom -> KnobLink
        sender = LrSocket.bind {
            functionContext = context,
            plugin = _PLUGIN,
            port = SEND_PORT,
            mode = 'send',
            onConnected = function(socket, port)
                log:trace('KnobLink connected (send ' .. port .. ')')
                sender_connected = true
                last_tool = nil   -- tell the new connection the current tool and module
                last_module = nil
                last_meta = nil
            end,
            onClosed = function(socket)
                sender_connected = false
                if SmartKnobRunning then socket:reconnect() end
            end,
            onError = function(socket, err)
                sender_connected = false
                if SmartKnobRunning then socket:reconnect() end
            end,
        }

        -- KnobLink -> Lightroom
        local receiver = LrSocket.bind {
            functionContext = context,
            plugin = _PLUGIN,
            port = RECEIVE_PORT,
            mode = 'receive',
            onConnected = function(socket, port)
                log:trace('KnobLink connected (receive ' .. port .. ')')
            end,
            onMessage = function(socket, message)
                for line in string.gmatch(message, '[^\r\n]+') do
                    local ok, err = pcall(handle, decode(line))
                    if not ok then log:trace('error: ' .. tostring(err) .. ' in ' .. line) end
                end
            end,
            onClosed = function(socket)
                if SmartKnobRunning then socket:reconnect() end
            end,
            onError = function(socket, err)
                if SmartKnobRunning then socket:reconnect() end
            end,
        }

        -- Value changes made any other way (mouse, new photo) show on the knob too
        pcall(function()
            LrDevelopController.addAdjustmentChangeObserver(context, {}, function()
                -- Mouse edits, new photo...; our own changes already reported themselves
                if os.clock() - last_set_time > 0.3 then
                    replySoon()
                end
            end)
        end)

        log:trace('SmartKnob plugin running')
        SmartKnobStatus = 'listening on ' .. RECEIVE_PORT .. ' and ' .. SEND_PORT
        LrDialogs.showBezel('SmartKnob ready')
        while SmartKnobRunning do
            if pending_full and inDevelop() then
                pending_full = false
                reportValue(current_param, true)
            end
            if pending_reply and os.clock() - last_reply_time > 0.1 then
                pending_reply = false
                last_reply_time = os.clock()
                reportValue(current_param, false)
            end
            -- Which module is showing (library, develop...)
            local module = LrApplicationView.getCurrentModuleName() or 'none'
            if module ~= last_module and sender_connected then
                log:trace('module: ' .. module)
                send({ t = 'module', module = module })
                last_module = module
            end

            -- Which tool is open (masking, healing...). Outside Develop: none.
            local tool = 'none'
            if inDevelop() then
                local ok, selected = pcall(LrDevelopController.getSelectedTool)
                if ok and selected then tool = selected end
            end
            if tool ~= last_tool and sender_connected then
                log:trace('tool: ' .. tool)
                send({ t = 'tool', tool = tool })
                last_tool = tool
            end
            -- The selected photo's rating and color label (for the Rating / Color pickers)
            local ok, photo = pcall(function() return LrApplication.activeCatalog():getTargetPhoto() end)
            if ok and photo and sender_connected then
                local rating = photo:getRawMetadata('rating') or 0
                local label = photo:getRawMetadata('colorNameForLabel') or ''
                local meta = rating .. '/' .. label
                if meta ~= last_meta then
                    send({ t = 'meta', rating = rating, label = label })
                    last_meta = meta
                end
            end
            LrTasks.sleep(TOOL_CHECK_SECONDS)
        end

        sender:close()
        receiver:close()
    end)
end)
