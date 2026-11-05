local ticks = 0
return {
    startup = function(host)
        assert(host.register_extension("org.nullus.sample.lua.counter", "runtime.play", function(event)
            if event == 1 then host.log("Lua runtime session begin") end
            if event == 2 then
                ticks = ticks + 1
                if ticks == 3 then host.log("Lua received third runtime session tick") end
            end
            if event == 3 then host.log("Lua runtime session end") end
            return ticks
        end))
        host.start_coroutine(function()
            while true do coroutine.yield() end
        end)
        host.log("Lua plugin registered with the host ExtensionRegistry")
    end,
    shutdown = function() end
}
