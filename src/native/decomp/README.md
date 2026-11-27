# Decompiled native code (guest-memory build) -- retail subset

Copied from sk83_na_m src/native/decomp (synced there from REXGLUE/skate-physics-decomp) for the
animation natives ported in 2026-11 (tools/sk83_port/PERF_PORT_2026-11.md). These sources were proven
bit-exact against the DEV build's recompiled functions; on retail every function must be proven
again against retail's own function (in-game differential verify) before it is enabled.
