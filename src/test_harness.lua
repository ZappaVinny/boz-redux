-- BOZ Test harness: built into the client and run before a test script (--test FILE), in the
-- test's environment (whose globals fall back to its mod's). The client gives `test` its
-- primitives (finish, finger, screenshot, errors, role, file, out); this adds the steps.
--
--   test.start_map("kino", {UnlimitedHealth = true})
--   test.step("the match starts", function()
--       test.wait_until(test.in_match, 120, "the match")
--   end)
--   test.step("doors", function()
--       test.expect(boz.get("Door1"):is_open() == false, "Door1 starts closed")
--   end)
--
-- Steps run one after another, inside a coroutine resumed every frame, so test.wait and
-- test.wait_until let the game run. A failed wait or an error ends the step; test.expect only
-- records. When every step has run, the client writes result.json and exits.

-- The client's functions, kept apart: test.screenshot and test.errors wrap them.
local primitives = {}
for name, value in pairs(test) do
    primitives[name] = value
end
local steps = {}
local results = {}
local elapsed = 0
local runner = nil
local current = nil
local finished = false
local started = false

local function fail_marker(message)
    return setmetatable({message = message}, {__tostring = function(self) return self.message end})
end

local function json(value)
    local kind = type(value)
    if kind == "table" then
        if #value > 0 or next(value) == nil then
            local items = {}
            for i, item in ipairs(value) do
                items[i] = json(item)
            end
            return "[" .. table.concat(items, ", ") .. "]"
        end
        local items = {}
        for key, item in pairs(value) do
            items[#items + 1] = json(tostring(key)) .. ": " .. json(item)
        end
        table.sort(items)
        return "{" .. table.concat(items, ", ") .. "}"
    elseif kind == "string" then
        return '"' .. value:gsub('[%c"\\]', function(c)
            if c == "\n" then
                return "\\n"
            end
            return string.format("\\u%04x", string.byte(c))
        end) .. '"'
    elseif kind == "number" or kind == "boolean" then
        return tostring(value)
    end
    return "null"
end

--- Seconds the test has been running (game frames, so a slow machine waits longer in real time).
function test.time()
    return elapsed
end

--- Adds a step: name and a function run when the steps before it are done.
function test.step(name, fn)
    if type(name) ~= "string" or type(fn) ~= "function" then
        error("test.step needs a name and a function", 2)
    end
    steps[#steps + 1] = {name = name, fn = fn}
end

--- Records a failure when ok is false (the step goes on). Returns ok.
function test.expect(ok, message)
    if not ok then
        message = message or "expectation failed"
        log("test: FAIL " .. (current and current.name or "?") .. ": " .. message)
        if current then
            current.failures[#current.failures + 1] = message
        end
    end
    return ok
end

--- Ends the step with a failure.
function test.fail(message)
    error(fail_marker(message or "failed"), 2)
end

--- Lets the game run for the given number of frames (default 1).
function test.frames(count)
    for _ = 1, count or 1 do
        coroutine.yield()
    end
end

--- Lets the game run for the given seconds.
function test.wait(seconds)
    local stop = elapsed + (seconds or 0)
    repeat
        coroutine.yield()
    until elapsed >= stop
end

--- Waits until fn() returns a true value (returned), or fails the step after seconds (default
--- 60); what names the wait in the failure.
function test.wait_until(fn, seconds, what)
    local stop = elapsed + (seconds or 60)
    while true do
        local ok, value = pcall(fn)
        if ok and value then
            return value
        end
        if elapsed >= stop then
            test.fail(string.format("timed out after %gs waiting for %s%s", seconds or 60,
                                    what or "a condition", ok and "" or (" (" .. tostring(value) .. ")")))
        end
        coroutine.yield()
    end
end

--- True when the mod's copy of the standard library sees a Zombies match with a local player.
function test.in_match()
    return require("boz.engine.map").player() ~= nil
end

local function vars_text(vars)
    local items = {}
    for name, value in pairs(vars or {}) do
        items[#items + 1] = name .. "=" .. tostring(value)
    end
    table.sort(items)
    return table.concat(items, ";")
end

local function autostart(name, vars, mode)
    -- The same one-shot settings the launcher and the SDK's Build & Run write.
    settings.set("autostart_level", name)
    settings.set("autostart_vars", vars_text(vars))
    settings.set("autostart_mode", mode)
    require("boz.engine.levels").autostart_from_settings()
end

--- Starts a single-player match on the level once the main menu is up (call it at the top of
--- the test); vars are console variables set first, e.g. {UnlimitedHealth = true}.
function test.start_map(name, vars)
    autostart(name, vars, nil)
end

--- Opens the Wi-Fi co-op room for the level (a co-op test: every client calls it; the host's
--- room opens first and the guests join it).
function test.coop_map(name, vars)
    autostart(name, vars, "coop")
end

--- Puts a finger down (id 0-9) at x, y (0..1 across the window), moves or lifts it. Fingers
--- reach the game in touch-screen mode only (the test's "--! touch: true").
function test.touch(id, action, x, y)
    primitives.finger(id, action, x or 0, y or 0)
end

--- A tap at x, y (0..1 across the window).
function test.tap(x, y)
    primitives.finger(9, "down", x, y)
    test.frames(3)
    primitives.finger(9, "up", x, y)
    test.frames(3)
end

--- Drags a finger from (x1, y1) to (x2, y2) over seconds (default 0.5), then lifts it unless
--- hold is true (then lift it with test.touch(9, "up")).
function test.drag(x1, y1, x2, y2, seconds, hold)
    seconds = seconds or 0.5
    primitives.finger(9, "down", x1, y1)
    test.frames(2)
    local start = elapsed
    while elapsed - start < seconds do
        local t = (elapsed - start) / seconds
        primitives.finger(9, "move", x1 + (x2 - x1) * t, y1 + (y2 - y1) * t)
        coroutine.yield()
    end
    primitives.finger(9, "move", x2, y2)
    if not hold then
        test.frames(2)
        primitives.finger(9, "up", x2, y2)
    end
    test.frames(2)
end

--- Saves the next frame as <name>.png in the report (name: letters, digits, - and _).
function test.screenshot(name)
    if not primitives.screenshot(name) then
        error("bad screenshot name " .. tostring(name), 2)
    end
    test.frames(2)
end

--- Lua errors raised by any mod so far.
function test.errors()
    return primitives.errors()
end

local function finish()
    finished = true
    local passed = true
    for _, result in ipairs(results) do
        passed = passed and result.passed
    end
    if #results == 0 then
        passed = false
        results[1] = {name = "(no steps)", passed = false, failures = {"the test has no steps"},
                      seconds = 0}
    end
    if primitives.errors() > 0 and not test.allow_errors then
        passed = false
    end
    log(string.format("test: %s after %.1fs", passed and "passed" or "FAILED", elapsed))
    primitives.finish(passed, json({steps = results, seconds = elapsed, role = test.role}))
end

local function run_steps()
    for _, step in ipairs(steps) do
        current = {name = step.name, passed = true, failures = {}, started = elapsed}
        log("test: step " .. step.name)
        local ok, err = xpcall(step.fn, function(e)
            if getmetatable(e) and e.message then
                return e.message
            end
            return debug and debug.traceback and debug.traceback(tostring(e), 2) or tostring(e)
        end)
        if not ok then
            current.failures[#current.failures + 1] = tostring(err)
            log("test: FAIL " .. step.name .. ": " .. tostring(err))
        end
        current.passed = #current.failures == 0
        current.seconds = elapsed - current.started
        current.started = nil
        results[#results + 1] = current
        current = nil
        if not ok and not test.keep_going then
            break
        end
    end
    -- Co-op: the host stays until the guests have left; leaving first would hand the match to a
    -- guest (host migration) while it still checks things.
    if test.role == "host" then
        local ok, network = pcall(require, "boz.engine.network")
        local stop = elapsed + 60
        while ok and elapsed < stop and #network.players() > 0 do
            coroutine.yield()
        end
    end
end

local function on_frame(dt)
    elapsed = elapsed + (dt or 0)
    if finished then
        return
    end
    if not started then
        started = true
        runner = coroutine.create(run_steps)
    end
    local ok, err = coroutine.resume(runner)
    if not ok then
        log("test: harness error: " .. tostring(err))
        results[#results + 1] = {name = "(harness)", passed = false, failures = {tostring(err)},
                                 seconds = elapsed}
        finish()
    elseif coroutine.status(runner) == "dead" then
        finish()
    end
end

events.on("frame", on_frame)
