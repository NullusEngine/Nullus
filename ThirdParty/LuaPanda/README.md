# LuaPanda 3.3.1

`Debugger/LuaPanda.lua` is the repository copy of the LuaPanda 3.3.1 pure-Lua
debugger script. Nullus uses the Lua 5.4 backend with `useCHook=false` and
never ships LuaPanda or `libpdebug` in Player/release packages.

The script is loaded only for an explicitly enabled Editor Play debug session.
Its socket dependency is intentionally a build-time project dependency; CMake
does not download LuaSocket or any other debugger package.
