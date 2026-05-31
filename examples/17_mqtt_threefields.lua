-- examples/17_mqtt_threefields.lua
--
-- Parses a 3-field sensor reading using the bundled json.decode and
-- re-emits an enriched JSON object containing the original fields plus
-- a "tags" array (HOT / HUMID / LOWP) and a server-side timestamp.
--
-- Downstream:
--   * the enriched JSON is republished on jerboa/env/alerts via an
--     `mqtt pub` sink;
--   * three filter_re branches match the quoted tag tokens
--     ("HOT" / "HUMID" / "LOWP") inside that JSON and fire one
--     exec action per field.
--
-- Input example:
--      {"temp":31,"hum":85,"pressure":980}
-- Output:
--      {"temp":31,"hum":85,"pressure":980,"tags":["HOT","HUMID","LOWP"]}
-- Or, if all readings are nominal:
--      {"temp":22,"hum":45,"pressure":1013,"tags":[]}
--
-- `json` is preloaded by the jerboa lua node from a bundled copy of
-- rxi/json.lua -- no install step required.

local TEMP_HOT = 28.0
local HUM_HIGH = 70.0
local PRES_LOW = 1000.0

function process(payload, _)
    local ok, t = pcall(json.decode, payload)
    if not ok or type(t) ~= "table" then return nil end
    if not (type(t.temp) == "number"
        and type(t.hum) == "number"
        and type(t.pressure) == "number") then
        return nil
    end

    local tags = {}
    if t.temp > TEMP_HOT then tags[#tags+1] = "HOT" end
    if t.hum  > HUM_HIGH then tags[#tags+1] = "HUMID" end
    if t.pressure < PRES_LOW then tags[#tags+1] = "LOWP" end

    t.tags = tags

    return json.encode(t)
end
