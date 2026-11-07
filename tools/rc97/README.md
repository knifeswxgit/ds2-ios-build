# RC97: optimised codegen (CPU)

1. `rexglue codegen` with `cr_as_local`, `ctr_as_local`, `xer_as_local`, `non_volatile_as_local` in `[entrypoint]`
   and `config/ds2_overrides.share.toml` in `includes` (after the other five).
2. `python3 lee_antes_ds2.py <gen>`: functions that read r14-r31/cr/ctr/xer before writing them. Ignore
   `setFromMask`, `const auto s_` and the `stvx128` prologue saves; mark the rest with `mark.py <list> config`
   (`share_registers = true`) and generate again until nothing is left.
3. `directas_ds2.py <gen>`: calls to functions without hooks become `__imp__sub_X` (inlinable).
4. `novolatile.py <gen>`: guest loads/stores without volatile (MMIO keeps it); compiler barrier on every loop
   back-edge and at sync/lwsync/eieio/isync.
Steps 3 and 4 must be repeated after every codegen.

## Lesson (RC97b crash)
`share_registers` on a normal function is wrong: in such a function the generator drops the
`__savegprlr_N` / `__restgprlr_N` calls (their bodies are no-ops once registers are locals), so it
clobbers ctx.r14-r31 and its caller copies the clobbered value back (RC97b: sub_82C2A9F8 returned
r31 = 0 to sub_82C2ACF8 -> null read at guest 0xD4). `lee_antes` is a linear scan and flags normal
functions too (a loop body placed before its init, `subfe rX,rY,rY`, partial vector writes).
Mark only pieces: `classify.py <gen>` -> PIECE (no own frame / prologue does not save the flagged
registers). Check with `shared_helpers.py <gen>`: no shared function may call a `__save*` helper.

## v102: vectors stay in ctx
`non_volatile_vector_as_local = false` (new generator option, default true): v14-v31 / v64-v127 are not
localized and `__savevmx` / `__restvmx` stay real calls. The Bink video decoder (hand-written VMX, 0x8317xxxx /
0x833Dxxxx) passes values between its functions in non-volatile vector registers; as per-function locals
they were lost and the intro video was striped purple/green in v100/v101 (fine in RC96).
