# Box2D

Nullus uses the official Box2D **3.2.0** C API.

- Source: https://github.com/erincatto/box2d
- Commit: `178495ba35695aefe9c321e569d2277f06db3b90`
- Archive SHA-256: `ed94013e8d16b19876757920bfa6d7663358aef8ba9407cc18146da6b714029e`
- Integration: `ThirdParty/box2d.cmake`
- License: MIT, retained beside this file and in the downloaded source.

Sources and SDK binaries stay in the build directory. For offline builds set
`NLS_BOX2D_SOURCE_DIR` to an unpacked 3.2.0 checkout. The adapter uses single
precision to match Nullus math, and never exposes backend IDs to game scripts.
