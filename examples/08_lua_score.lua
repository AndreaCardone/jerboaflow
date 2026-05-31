-- examples/08_lua_score.lua
--
-- Per-input transform that:
--   1) keeps a running counter of packets seen
--   2) extracts the HTTP status code from a log line like
--          ... "GET /foo" 404 0
--   3) emits "<count>: <status>" (e.g. "3: 404")
--   4) drops lines that don't look like log entries (returns nil)
--
-- Stateful: `count` is a Lua global, preserved across calls because the
-- per-node lua_State lives for the lifetime of the flow.

count = 0

function process(payload, in_idx)
    local status = payload:match('"%u+ [^"]+"%s+(%d+)%s+%d+')
    if not status then
        return nil    -- not a recognizable log line; drop
    end
    count = count + 1
    return string.format("%d: %s", count, status)
end
