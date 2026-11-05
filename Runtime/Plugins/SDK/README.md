# Nullus native Binary Plugin SDK, ABI 1

This SDK contains only C declarations and CMake validation. Never link a Binary
Plugin to an engine implementation library. Source Plugins are a separate,
same-toolchain STATIC/OBJECT integration using `ModuleContext`.

```cmake
cmake_minimum_required(VERSION 3.22)
project(MyPlugin LANGUAGES CXX)
include("${NLS_PLUGIN_SDK_DIR}/NullusPluginSDK.cmake")
nullus_add_binary_plugin(MyPlugin MyPlugin.cpp)
```

Export exactly `NullusPlugin_QueryApi` with `NLS_PLUGIN_EXPORT` and
`NLS_PLUGIN_CALL`. Copy at most the caller's `struct_size`. Major versions must
match; minor versions append fields. Layout is pack(8), fixed-width integers,
UTF-8 pointer/length strings and opaque owner handles; never expose STL objects,
C++ exceptions or compiler RTTI. The shipped header verifies 64-bit offsets and
also compiles as C11. Current supported binary targets are little-endian x64 and
arm64, Windows PE, Linux ELF and thin macOS Mach-O; universal Mach-O is rejected.

All registration and lifecycle calls run on the host main thread. Only `log`
and the paired host `allocate/free` functions allow worker threads. Log includes
owner and phase. Input strings are borrowed for the call; registration metadata
is copied. A successful extension registration transfers callback-context
ownership to its `destroy`; failure leaves it with the caller. The plugin owns
its instance and destroys it through its own `destroy`. Never free memory with a
different allocator or use a host table/owner after session revocation.

PreEngine has no resource reader or graphics/editor services. Runtime raw reads
use caller buffers and report the required size. Declare retained paths in the
content mount's `runtimeKeep`; dynamic strings are not discovered by Cook.
Dependent libraries must be listed in `runtimeDependencies`, shipped in the
plugin's declared binary directories and built for the target. Plugins own their
C++ runtime allocations; no host CRT object crosses the ABI.

Startup may partially fail. Host registrations roll back automatically; private
threads and external resources must cooperate with `request_stop`,
`abort_startup` and `is_stopped`. These functions and `shutdown/destroy` must not
throw. Every independently executing operation must be accounted for before
`is_stopped` returns OK. `request_unload` queues work and returns immediately,
including when called inside a plugin callback. A lease cannot bypass the call
gate after Quiescing.

Restart application is the default enable/update/disable guarantee. Hot unload
requires both the manifest flag and ABI capability, successful drain and full
cleanup. Unknown references, timeouts and cleanup failures pin the mapping until
process exit. A cleaned module's catalog/factory can still retain its mapping;
the current host does not promise immediate unmapping. Do not wait synchronously
for self-unload. The engine cannot
isolate or verify arbitrary native code; installation never executes package
build scripts, while explicit source-build approval authorizes those scripts.
