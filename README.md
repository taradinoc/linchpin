# Linchpin

This repo contains a mixed C# and C codebase for working with the Cornerstone virtual machine.
It includes:

- C# tools for assembling, inspecting, running, disassembling, and repackaging Cornerstone images.
- Native runtimes for Commodore 128, Atari ST, Amiga, and POSIX hosts.
- Bundled sample data.

## Repository layout

- `Cornerstone/` - install Cornerstone here before running the test suite.
- `data/` - shared instruction grammar used by the assembler and C# interpreter.
- `DB/` - database files used for testing.
- `src/Chisel/` - C# assembler that turns `.cas` source into `.obj` and `.mme` images.
- `src/Linchpin/` - C# interpreter and disassembler for Cornerstone images.
- `src/LP128Pack/` - C# bundler that packages `.mme` + `.obj` into a `.csb` bundle for the C128 runtime.
- `src/Linchpin128/` - C runtime for POSIX and Commodore 128 targets.
- `src/LinchpinST/` - C runtime for POSIX, Atari ST, and Amiga targets.
- `test/Linchpin.ToolTests/` - MSTest suite for the C# tools.
- `test/Linchpin.TestCaseTool/` - helper for creating interpreter-based test cases.

## Projects

### C# projects

#### `src/Chisel`

Assembler for the Cornerstone VM. It reads `.cas` source and produces `.obj` and `.mme` output files.

Example source files are under `src/Chisel/examples/`.

#### `src/Linchpin`

Interpreter and disassembler for Cornerstone images. Main commands:

- `run` - execute the VM.
- `inspect` - print image metadata and an entrypoint preview.
- `disassemble` - emit reconstructed assembler source.

#### `src/LP128Pack`

Packs a Cornerstone `.mme` and `.obj` pair into a `.csb` bundle for the Linchpin128 runtime.

### Native C projects

#### `src/Linchpin128`

Native runtime for the Cornerstone VM.

- `make posix` builds a POSIX-hosted executable.
- `make c128` builds a Commodore 128 program with `llvm-mos`.
- `make c128-d64`, `make c128-d71`, and `make c128-d81` also create disk images.

The C128 disk-image targets use VICE `c1541.exe` from WSL and can optionally embed a `.csb` bundle.

#### `src/LinchpinST`

Native runtime for multiple classic targets.

- `make posix` builds a POSIX-hosted executable.
- `make atari` / `make atari-tos` build Atari binaries.
- `make atari-disk` builds an Atari ST disk image.
- `make amiga`, `make amiga-adf`, and `make amiga-hdf` build Amiga binaries and disk images.
- `make amiga-fpu` and related targets build the 68040/FPU Amiga variant.

`src/LinchpinST/LinchpinST.proj` is an MSBuild wrapper that calls `wsl make`, but the actual native compilation still happens inside WSL.

## Prerequisites

### Windows host

- Visual Studio or Build Tools with MSBuild support.
- .NET SDK 10.0 for the C# projects.
- WSL for all native C builds.

### WSL tools

At minimum:

- `make`
- `gcc` for POSIX builds
- `python3` for the Commodore 128 launcher generator

Additional target-specific tools:

- `llvm-mos` for `src/Linchpin128` Commodore 128 builds
- VICE `c1541.exe` reachable from WSL for Commodore disk-image targets
- `m68k-atari-mintelf-gcc` or `m68k-atari-mint-gcc` for Atari builds
- `dosfstools` and `mtools` for Atari disk-image generation
- `m68k-amigaos-gcc` for Amiga builds
- `amitools` (`xdftool`) for Amiga ADF/HDF image generation

## Building

### Build the C# projects

Build the solution:

```powershell
dotnet build linchpin.slnx
```

Build an individual project:

```powershell
dotnet build src\Linchpin\Linchpin.csproj
dotnet build src\Chisel\Chisel.csproj
dotnet build src\LP128Pack\LP128Pack.csproj
```

### Build the native C projects

If you are invoking the build from Windows, run these commands inside WSL.

POSIX builds:

```sh
cd src/LinchpinST
wsl make clean
wsl make
```

Target-specific examples:

```sh
# Build Linchpin128 as a Commodore 128 disk image
# with a bundled bytecode program
cd src/Linchpin128
make c128-d81 BUNDLE=path/to/bundle.csb

# Build LinchpinST as an Atari ST disk image
cd src/LinchpinST
make atari-disk

# Build LinchpinST as an Amiga disk image
cd src/LinchpinST
make amiga-adf
```

## Running

### Linchpin

Inspect an image:

```powershell
Linchpin inspect --mme path/to/image.mme
```

Run an image with a database directory, enabling writes:

```powershell
Linchpin run --mme path/to/image.mme --data YourDB --host-write-files
```

Disassemble an image to a file:

```powershell
Linchpin disassemble --mme path/to/image.mme --output path/to/image.cas
```

### Chisel

Assemble an example source file:

```powershell
Chisel assemble src/Chisel/examples/hello-world.cas --mme hello-world.mme
```

### LP128Pack

Package an assembled image into a bundle for use with the Linchpin128 build:

```powershell
LP128Pack --mme hello-world.mme --obj hello-world.obj --output hello-world.csb
```

## Testing

Run the test suite with:

```powershell
dotnet test
```
