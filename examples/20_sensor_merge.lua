-- examples/20_sensor_merge.lua
--
-- Merge one reading from each of three sensor files into one JSON object.
-- Input port mapping:
--   0 -> temperature (C)
--   1 -> humidity (%)
--   2 -> smoke (0/1)

local queue = {
    {},
    {},
    {},
}

local sample = 0

local function push(q, v)
    q[#q + 1] = v
end

local function pop(q)
    local v = q[1]
    table.remove(q, 1)
    return v
end

local function ready()
    return #queue[1] > 0 and #queue[2] > 0 and #queue[3] > 0
end

local function alarm_type(temp, humidity, smoke)
    if smoke > 0 then
        return "smoke_detected"
    end
    if temp > 28 then
        return "high_temperature"
    end
    if humidity > 70 then
        return "high_humidity"
    end
    return nil
end

function process(payload, in_idx)
    local value = tonumber(payload)
    if not value then
        return json.encode({ error = "bad_sensor_value", raw = payload, port = in_idx })
    end

    local slot = in_idx + 1
    if slot < 1 or slot > 3 then
        return json.encode({ error = "bad_sensor_port", port = in_idx })
    end

    push(queue[slot], value)
    if not ready() then
        return nil
    end

    local temp = pop(queue[1])
    local humidity = pop(queue[2])
    local smoke = pop(queue[3])
    sample = sample + 1

    local obj = {
        sample = sample,
        payload = {
            temperature = { value = temp, unit = "C" },
            humidity = { value = humidity, unit = "pct" },
            smoke = { value = smoke, unit = "bool" },
        },
    }

    local kind = alarm_type(temp, humidity, smoke)
    if kind then
        obj.alarm = true
        obj.alarm_type = kind
    end

    return json.encode(obj)
end
