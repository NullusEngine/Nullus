# PhysX CPU SDK

The engine builds PhysX 5.6.1 from the fixed upstream source commit
`09ff24f3279b735e672ff27b155cbf49f6296f4d`.

Source: https://github.com/NVIDIA-Omniverse/PhysX

Archive SHA-256: `d6d112ae8ed3de7c0478ef87d49e5423c8a364d72b2ba5d349115d57e863b6e2`.

The source and build products live in the CMake build directory. Set
`NLS_PHYSX_SOURCE_DIR` to an existing checkout to configure without downloading.
The SDK is copied into a private build directory before local patches are applied.

Local integration changes:

- Resolve the macOS `size_t` ambiguity in temporary-allocation bit scans with an explicit 64-bit argument.
- Disable the sample FreeGLUT DLL copy for the CPU SDK.
- Compile the SDK as C++14 with the same dynamic CRT as the engine.
- Isolate SDK build flags and preserve Nullus configurations.
- Use static, position-independent SDK libraries; the physics module exposes engine types.
- Add an Apple CMake platform from the Unix implementation, adapt thread ID lookup
  to pthread_threadid_np and remove Linux-only linker libraries. Device validation remains pending.

The pinned release uses the BSD 3-Clause license. See LICENSE.md.
