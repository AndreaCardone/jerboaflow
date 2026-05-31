-- examples/16_mqtt_json.lua
--
-- Parses a sensor reading published as JSON over MQTT, e.g.
--      {"sensor":"kitchen","temp":28.5,"hum":40}
-- and emits a tagged single-line summary:
--      ALERT sensor=kitchen temp=28.5
--      OK    sensor=bedroom  temp=21.0
--
-- Downstream filter nodes split on the ALERT/OK tag so each branch can
-- trigger a different action.
--
-- The Lua sandbox in jerboa has no cjson; for fixed-shape messages a
-- couple of patterns are enough. For richer shapes drop in a pure-Lua
-- decoder (e.g. rxi/json.lua) -- it works unmodified under the sandbox.

local THRESHOLD = 27.0   -- degrees celsius

function process(payload, _)
    local sensor = payload:match('"sensor"%s*:%s*"([^"]+)"')
    local temp_s = payload:match('"temp"%s*:%s*(%-?[%d%.]+)')
    local temp   = tonumber(temp_s)

    if not sensor or not temp then
        return nil    -- malformed / unrelated message: drop silently
    end

    local tag = (temp > THRESHOLD) and "ALERT" or "OK"
    return string.format("%s sensor=%s temp=%.1f", tag, sensor, temp)
end
