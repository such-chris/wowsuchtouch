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
local TOOL_CHECK_SECONDS = 0.25

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
local function valueText(param, value)
    local low, high = LrDevelopController.getRange(param)
    if low and high and (high - low) <= 20 then
        return string.format('%+.2f', value)
    elseif low and low < 0 then
        return string.format('%+d', math.floor(value + 0.5))
    else
        return string.format('%d', math.floor(value + 0.5))
    end
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
        local last_tool = nil
        local last_module = nil

        local function send(message)
            if sender and sender_connected then
                local ok, err = pcall(function() sender:send(encode(message) .. '\n') end)
                if not ok then log:trace('send failed: ' .. tostring(err)) end
            end
        end

        local function reportValue(param)
            if not param or not inDevelop() then return end
            local value = LrDevelopController.getValue(param)
            if type(value) == 'number' then
                send({ t = 'val', param = param, v = value, txt = valueText(param, value) })
            end
        end

        local function handle(message)
            if message.t == 'turn' and message.param then
                current_param = message.param
                if not inDevelop() then return end
                local steps = message.d or 0
                if steps ~= 0 then
                    local low, high = LrDevelopController.getRange(message.param)
                    if not low then return end
                    local step = message.step or ((high - low) / 200)   -- 200 detents end to end
                    local value = LrDevelopController.getValue(message.param) + steps * step
                    value = math.max(low, math.min(high, value))
                    LrDevelopController.setValue(message.param, value)
                end
                reportValue(message.param)
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
                LrDevelopController.resetToDefault(message.param)
                reportValue(message.param)
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
                reportValue(current_param)
            end)
        end)

        log:trace('SmartKnob plugin running')
        SmartKnobStatus = 'listening on ' .. RECEIVE_PORT .. ' and ' .. SEND_PORT
        LrDialogs.showBezel('SmartKnob ready')
        while SmartKnobRunning do
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
            LrTasks.sleep(TOOL_CHECK_SECONDS)
        end

        sender:close()
        receiver:close()
    end)
end)
