# Linchpin Project Guidelines

## Architecture

Mixed C# (.NET 10) and native C codebase implementing the **Cornerstone virtual machine** for multiple targets.

**C# tools** (all `net10.0`, single-namespace, all types `internal`):
- `src/Chisel/` — Assembler: `.cas` source → `.obj` + `.mme` images
- `src/Linchpin/` — Interpreter and disassembler for `.mme` images
- `src/LP128Pack/` — Bundler: packs `.mme` + `.obj` into `.csb` for the C128 runtime

**Native C runtimes** (built inside WSL):
- `src/LinchpinST/` — Atari ST, Amiga, and POSIX runtime (`lpst_` prefix)
- `src/Linchpin128/` — Commodore 128 and POSIX runtime (`lp128_` prefix)

**Tests:**
- `test/Linchpin.ToolTests/` — MSTest integration suite; runs Linchpin CLI and validates output
- `test/Linchpin.TestCaseTool/` — CLI helper for authoring JSON test-case manifests

**Shared data:** `data/instruction_grammar.json` is embedded as a gzip-compressed resource into both `Chisel` and `Linchpin` at build time (see `Directory.Build.targets`).

## Build and Test

```powershell
# Build all C# projects
dotnet build linchpin.slnx

# Run C# tests (requires Cornerstone/ to be populated)
dotnet test

# Build native POSIX executables (must run from Windows host)
wsl make -C src/LinchpinST posix
wsl make -C src/Linchpin128 posix

# Build target-specific images
wsl make -C src/LinchpinST atari-disk
wsl make -C src/LinchpinST amiga-adf
wsl make -C src/Linchpin128 c128-d81 BUNDLE=path/to/bundle.csb
```

See [README.md](../README.md) for the full prerequisite list and all make targets.

## Key Conventions

### C#
- All types are `internal`; prefer `sealed` and `readonly` where applicable.
- Use **records** for value/data types (e.g., `CornerstoneImage`, `VmState`, `DecodedInstruction`).
- Private fields: `_camelCase`.
- Fully nullable-aware; always annotate with `?`, never use `null!` to silence warnings without a comment.
- Large classes (e.g., `VmRuntimeState`) split into partial files by subsystem.
- Domain errors use `LinchpinException`; let standard exceptions propagate for system-level failures.
- Chisel nests most of its types inside `internal static partial class Program`.

### C (native)
- `snake_case` everywhere; module prefix for all identifiers (`lpst_` or `lp128_`).
- Use explicit-width types (`uint8_t`, `uint16_t`, `uint32_t`); avoid bare `int` for VM data.
- Struct type names end in `_s` (e.g., `lpst_exec_state`).
- Macros are `UPPER_CASE`.
- Platform guards: `#ifdef __mos__` / `#ifdef __llvm_mos__` for C128-only code; POSIX fallback otherwise.
- Large structures (VM state, bundles) are statically allocated to avoid stack pressure on 8-bit targets.

## Documentation

- `doc/vm-specification.md` — Normative Cornerstone VM specification (authoritative reference for opcodes, encoding, memory model).
