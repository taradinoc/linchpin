# Cornerstone Virtual Machine Specification

## Version 0.2

*Tara McGrew*\
*March&ndash;April 2026*

## 1. Introduction

### 1.1 Purpose

This document is the normative specification for the **Cornerstone VM**, a
16-bit bytecode virtual machine. It defines the contract between an interpreter
and the program it executes.

> **Note:** The Cornerstone VM was historically known as the Mu Machine. The
> original implementation is **MME** (`MME.EXE`), an abbreviation for Mu Machine
> Emulator.

This specification serves as a reference for:

1. parsing `.MME` and `.OBJ` image files;
2. disassembling valid Cornerstone bytecode images; and
3. implementing an interpreter capable of accurately executing the Cornerstone
   database package and other existing program images.

### 1.2 Scope

A complete Cornerstone VM executable image consists of:

1. a `.MME` file, containing the image header, metadata tables describing
   modules and their contents, and the initial RAM image; and
2. a `.OBJ` file, containing the modules and read-only data.

The machine provides a stack-based execution architecture with procedures,
local variables, program globals, module globals, vectors, tuples, and
non-local control transfer, as well as host services including memory
allocation, display control, keyboard polling, stream- and record-based I/O,
and data searching and extraction. All of the above is within the scope of
this specification.

The binary formats of Cornerstone application files — including `.DBF`
database record files, field index files, and other file structures read or
written by a running Cornerstone program — are outside the scope of this
specification. Those formats are transferred as opaque byte streams by the
interpreter's I/O opcodes; this specification defines the I/O interface, not
the content transferred through it.

### 1.3 Normative Language

The key words **must**, **must not**, **shall**, **shall not**, **should**,
**should not**, **recommended**, **may**, and **optional** in this document
are to be interpreted as described in RFC 2119.

Unless explicitly marked otherwise, all statements in numbered sections are
normative. Non-normative content appears in **Note**, **Rationale**, or
**Provisional** callouts.

Behavior that this specification does not define is **undefined**; no
conforming program may rely on it, and an interpreter may take any action in
response. Behavior that this specification explicitly delegates to the
implementation is **implementation-defined**; an interpreter must behave
consistently for any given implementation-defined choice and must document
that choice.

A **conforming program** is one whose execution does not rely on any
behavior that this specification designates as undefined.

A **conforming interpreter** is one that implements all normative requirements
of this specification for every conforming program.

### 1.4 Notation and Conventions

In this specification:

- Hexadecimal constants are written in C syntax with uppercase hex digits:
  `0xAB`, `0x5F`.
- **Bold** marks the primary definition of a term or structure.
- *Italic* marks emphasis.
- Offsets are in bytes unless otherwise stated.
- Values are words unless otherwise stated.
- This specification uses ordinal numbers when referring to elements subject
  to multiple indexing conventions. For example, the initial word of an
  aggregate — which may have index `0` or `1` depending on the accessor — is
  described as the "first word".

The following terms are used throughout this specification. Each is defined
briefly here; entries that span multiple chapters are given complete normative
definitions at their canonical location, which is noted.

- A **word** is a 16-bit little-endian value. All logical addresses and all
  VM storage values are words unless stated otherwise.
- A **byte** is an 8-bit value.
- **Bits** within a byte are numbered 0 (least significant) to 7 (most
  significant).
- A **module number** is 1-based.
- An **index** is 1-based unless otherwise stated.
- An **offset** is zero-based.
- A **logical address** is a 16-bit word address in the VM's split address
  space. See section 4.1 for the complete definition including segment
  boundaries and the address-to-physical mapping.
- An **aggregate** is a contiguous group of words in VM RAM accessed through
  a **handle**. Vectors, tuples, and static arrays in the initial RAM image
  are all aggregates. See section 4.3.
- A **handle** is a logical address pointing to the first word of an
  aggregate. A handle value of FALSE (see section 1.5.1) indicates the absence
  of an aggregate and must not be dereferenced. A handle is not a channel
  number.
- A **vector** is an aggregate allocated from the vector heap; see section 4.3.
- A **tuple** is an aggregate allocated from the tuple stack; see section 4.4.
- A **string** is an aggregate whose layout is defined in section 4.6.
- The **evaluation stack** is the per-call-frame stack of words from which
  instructions consume their inputs and to which they write their results;
  see section 2.1.
- A **procedure** is a block of bytecode in a module, identified by a
  procedure offset; see section 6.1.
- A **procedure selector** is a 16-bit value encoding a far-call target. See
  section 3.2.1 for the compact field layout and section 6.3 for the full
  normative definition and resolution rules.
- The **Cornerstone VM**, or **the VM**, is the abstract machine defined by
  this specification.
- An **interpreter** is any implementation of the VM described in this
  specification.
- The **program** is the software executing within the VM. Use *program* in
  normative and behavioural prose.
- **Bytecode** is the encoded form of the instruction stream and associated
  data as it exists in the `.MME` and `.OBJ` files. Use *bytecode* when
  discussing instructions as data rather than as executing code.
- **RAM** is the word-addressed memory made available to the program at
  runtime; see section 4.1.
- A **VM-visible** value or behavior is one that a conforming program can
  observe or depend on.

### 1.5 Fundamental Values and Behavioral Conventions

#### 1.5.1 The FALSE Sentinel

**FALSE** is the value `0x8001`. FALSE is not zero and is not related to the
word value `0x0000`.

The VM uses FALSE as the canonical absent-or-failed result. Specifically:

1. A local variable that is not explicitly initialized by the procedure header
   has the value FALSE at procedure entry (see section 6.4).
2. Opcodes that perform a search or lookup and find no match push FALSE onto
   the evaluation stack (see the applicable entries in Appendix B).
3. `KBINPUT` returns FALSE when no key is pending (see section 8.3).
4. A handle value of FALSE indicates the absence of an aggregate. Opcodes that
   accept an optional aggregate may receive FALSE to indicate "none"; the
   behavior of each such opcode is defined in Appendix B.

Passing FALSE as a handle to any opcode that dereferences it produces
undefined behavior.

#### 1.5.2 The Signed Comparison Result Convention

Several opcodes return a **signed comparison result**: the word value `+1` if
the first operand is strictly less than the second operand, `0` if the
operands are equal, and `−1` if the first operand is strictly greater than the
second operand.

> **Note:** This convention is the **opposite** of the C `memcmp`/`strcmp`
> sign convention, in which a positive result indicates that the first argument
> ranks higher than the second.

The opcodes that return a signed comparison result are identified in Appendix B
and include `MEMCMP`, `MEMCMPO`, `KEYCMP`, `STRICMP`, and `STRICMP1`. Those
Appendix B entries cross-reference this section rather than restating the
convention.

### 1.6 Reference Implementations and Completeness

**MME** (`MME.EXE`) is the original DOS implementation of the Cornerstone VM
and is the authoritative reference for VM behavior.

Two additional reference implementations exist: **Linchpin** (C#) and
**LinchpinST** (C). Their source is publicly available and may be consulted as
a supplement when this specification is found to be incomplete. See the
Linchpin project repository for the current source location.

> **Note:** Where this specification is found to describe behavior that
> contradicts MME's observed behavior, the specification is likely in error.
> Such discrepancies should be reported as bugs against the specification.

This specification may be incomplete. Existing program images may depend on
interpreter behavior that has not yet been documented here. Future versions of
this specification may define behavior that this version leaves undefined.

## 2. Architectural Overview

This chapter provides a conceptual map of the Cornerstone VM for a reader
encountering it for the first time. The content here is informative; normative
requirements are stated in the chapters that follow. Each subsection identifies
where those normative details live.

### 2.1 Machine Model

The Cornerstone VM is a stack machine. Instructions consume operands from an
**evaluation stack** of words and deposit results either onto that stack or
into one of five storage spaces:

1. **local variables** in the active call frame;
2. **program globals**, shared across all modules;
3. **module globals**, private to the module that accesses them;
4. **aggregates** (vectors, tuples, and static arrays; see sections 4.3–4.4); and
5. **system variables**, accessed as module-global indices `0xC0`–`0xFF`
   (see section 7).

All VM values are 16-bit words.

The tuple stack (section 4.4) is a separate runtime structure used to allocate
tuples; it is distinct from the evaluation stack.

The VM interacts with the host environment for display output, keyboard input,
and file I/O. Host services are described in section 8.

### 2.2 Module System

A program is partitioned into **modules**. All modules reside in the `.OBJ`
file; module start offsets are recorded in the module table in the `.MME` file.
Each module has its own set of module globals and may export procedures to
other modules.

Procedures within the same module call each other using **near calls**, which
identify the target procedure by its **procedure offset**—a module-relative
byte address.

To call a procedure in a different module, the caller uses a **far call**, which
identifies the target by a **procedure selector**. A procedure selector is a
16-bit value that encodes the target module and the target procedure's position
in that module's export table (see section 3.2.1 for the binary encoding and
section 6.3 for the normative call-form rules). For a far call to succeed, the
target procedure must appear in that module's **export table** in the `.MME`
file.

> **Note:** The normative limits on the number of modules, the number of
> procedures exported per module, and the maximum code size per module are
> defined in section 3.1.

### 2.3 Fetch-Dispatch Loop

The interpreter fetches one byte at a time from the active instruction stream.

Bytes `0x00`–`0x5E` are **base opcodes** and are dispatched immediately. Byte
`0x5F` is the **extended-opcode** prefix; the interpreter fetches a second byte
to form the secondary opcode. The defined secondary-opcode range is
`0x00`–`0x38`. Bytes `0x60`–`0xFF` encode the packed opcode families, in which
the complete instruction—including its operand—is encoded in a single byte.
Full encoding rules for all opcode forms are defined in section 5.

Execution within a module is organized into 256-byte **code blocks** terminated
by `NEXTB`; see section 5.2 for the block discipline.

## 3. Image File Format

### 3.1 Overview

A Cornerstone VM program is distributed as two binary files: an `.MME` file
and an `.OBJ` file. The `.MME` file contains the image header, module table,
program global tables, module export tables, and the **initial RAM image** that
is loaded into VM RAM at startup. The `.OBJ` file contains the executable
**bytecode** of each **module** and an optional **read-only data region**.

The following capacity limits apply to all conforming image files:

| Quantity | Limit |
|----------|-------|
| Modules | ≤ 24 |
| Program globals | ≤ 256 (indices 0–255) |
| Module globals per module | ≤ 192 (indices `0x00`–`0xBF`) |
| Exported procedures per module | ≤ 255 |
| Code size per module | ≤ 65,536 bytes (64 KiB) |
| Initial RAM image size | ≤ 32,768 words (64 KiB) |

The authoritative statement of each limit appears in the subsection that
defines the relevant structure. The table above is provided as a convenience
summary only.

### 3.2 The `.MME` File

The `.MME` file is the primary metadata container for a Cornerstone program.
Its layout, from lowest to highest file offset, is: image header, module table,
program global count table, program global initial-value table, module export
tables (at an offset given by the header), padding bytes, and initial RAM
image.

#### 3.2.1 Image Header

The `.MME` file begins with a 48-word (96-byte) **image header**. All header
fields are little-endian words. The following fields are defined:

| Byte offset | Meaning |
|:-----------:|---------|
| `0x0E` | Total size in bytes of all modules in the `.OBJ` file, divided by 512 and rounded down |
| `0x10` | **Procedure selector** of the program entry point (see Note below) |
| `0x16` | Size in words of the initial RAM image (see section 3.2.5) |
| `0x1A` | Offset in the `.MME` file of the module export tables, divided by 256 (see section 3.2.4) |
| `0x1E` | Length of the module table in words (see section 3.2.2) |

All other bytes in the image header are reserved. Image emitters should fill
reserved header bytes with zero.

> **Note:** A **procedure selector** is a 16-bit value whose high byte is the
> 1-based module number and whose low byte is the zero-based index into that
> module's export table. The entry-point field at `0x10` gives the procedure
> that the interpreter shall call when the program starts. The full normative
> definition of procedure selector encoding and resolution is in section 6.3.

The initial portion of the header at byte offsets `0x00`–`0x0C` is the
**image preamble**. Its internal semantics are not defined by this specification.

> **Note:** An image emitter should produce the following preamble bytes:
> `0x65, 0x00, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x16, 0x00,`
> `0xAB, 0x00`. This sequence matches the preamble used by programs known to
> run correctly under MME; the interpreter's behavior for other preamble
> values is not defined by this specification.

#### 3.2.2 Module Table

The image header is followed immediately at byte offset `0x60` by the **module
table**, which lists the location of each module in the `.OBJ` file. The module
table contains one entry for each module in order, preceded by a count word:

| Table offset | Meaning |
|:------------:|---------|
| `+0` | Number of modules |
| `+2` | Offset of module 1 in the `.OBJ` file, divided by 256 |
| … | … |
| `+2N` | Offset of module N in the `.OBJ` file, divided by 256 |

The number of modules must not exceed 24.

The length of the module table in words is given by the image header field at
`0x1E`. A parser may use this field to determine where the module table ends
and the program global count table begins.

> **Note:** Module numbers in the `.MME` metadata structures are 1-based.
> Module 1 is the first entry in the module table.

#### 3.2.3 Program Global Tables

The module table is followed by the **program global count table**, which
records the number of program globals and the number of module globals for each
module:

| Table offset | Meaning |
|:------------:|---------|
| `+0` | Number of **program globals** |
| `+2` | Number of **module globals** for module 1 |
| … | … |
| `+2N` | Number of module globals for module N |

The number of program globals must not exceed 256. Program globals are indexed
from 0 through 255.

The number of module globals per module must not exceed 192. Module globals for
each module are indexed from `0x00` through `0xBF`. Indices `0xC0`–`0xFF` are
reserved for system variables and must not be declared as module globals.

The program global count table is followed by the **program global
initial-value table**, which supplies the startup value of every program global
and module global, in the same order as the count table:

| Table offset | Meaning |
|:------------:|---------|
| `+0` | Initial values of program globals 0–(N−1), one word each, in order |
| | Initial values of module globals for module 1, one word each, in order |
| … | … |
| | Initial values of module globals for module M, one word each, in order |

#### 3.2.4 Module Export Tables

The module export tables begin at the file offset (multiplied by 256) given by
the image header field at `0x1A`. One export table is present for each module,
in ascending module-number order. Each export table has the following layout:

| Table offset | Meaning |
|:------------:|---------|
| `+0` | Number of **procedure offsets** exported by the module |
| `+2` | **Procedure offset** of exported procedure 1 within the module |
| … | … |
| `+2N` | Procedure offset of exported procedure N within the module |

The number of exported procedures per module must not exceed 255. Exports are
indexed from 1 in this table; a procedure selector's low byte refers to a
zero-based index into this list, so the export at table position 1 corresponds
to selector low byte `0x00`.

The last module export table must be followed by at least one padding byte
before the initial RAM image.

#### 3.2.5 Initial RAM Image

The **initial RAM image** begins at the first 256-byte boundary in the `.MME`
file that follows the padding byte(s) after the last module export table. If
the last export table ends exactly on a 256-byte boundary, 256 additional
padding bytes must precede the initial RAM image.

The size of the initial RAM image in words is given by the image header field
at `0x16`. The initial RAM image must not exceed 32,768 words (64 KiB).

> **Note:** The initial RAM image should be kept as small as practical. The
> split address-space model (see section 4.1) means that if the initial RAM
> image is larger than the memory allocated to the low RAM segment at runtime,
> the excess is placed in the high RAM segment, which invalidates the compiled
> word addresses of any data structures located in the overflow region.

### 3.3 The `.OBJ` File

The `.OBJ` file contains the executable bytecode of all modules. Each module
must begin at a 256-byte boundary within the `.OBJ` file. The starting offset
of each module is given by the corresponding entry in the module table
(section 3.2.2), multiplied by 256.

> **Note:** This 256-byte module boundary is a file-level placement
> constraint. It is distinct from the instruction-level code block alignment
> rule, which requires each 256-byte code block within a module to be
> terminated by a `NEXTB` instruction. See section 5.2 for the code block
> structure rules.

The modules may be followed by a **read-only data region**. The interpreter
shall make the read-only data region available to the program as a readable
pseudo-file; the channel through which it is accessible is determined by
system variable `0xDA` (see section 7).

## 4. Memory Model

This chapter defines the VM's runtime address space, the two dynamic memory
allocators, the access conventions for aggregate data structures, and the
in-RAM representations for strings and floating-point values. The concepts
defined here are prerequisites for section 5 (Instruction Stream and Encoding)
and section 6 (Procedures and Execution).

### 4.1 Logical Address Space

The VM exposes a 16-bit logical **word-address space** divided into two 64-KiB
segments: the **low RAM segment**, covering logical addresses `0x0000–0x7FFF`,
and the **high RAM segment**, covering logical addresses `0x8000–0xFFFF`. A
**logical address** is a 16-bit value interpreted as a word address within this
space.

A logical address is decoded as follows:

1. Bit 15 selects the segment: `0` selects the low RAM segment; `1` selects the
   high RAM segment.
2. Bits 14–0 identify the word slot within the selected segment. The word at that
   slot resides at byte offset `2 × (address & 0x7FFF)` within the segment's
   physical storage.
3. Byte access uses the same segment-selection rule and then applies a byte
   displacement within the word-addressed space.

The notional capacity of the address space is 128 KiB. At runtime, the physical
memory available to the VM may be less than 128 KiB. In that case, each segment
is backed by an equal share of the available physical memory, beginning at its
respective base address. Logical addresses that exceed the physical extent of
their segment form a gap; the result of reading or writing a logical address in
this gap is undefined.

> **Note:** The maximum size of the initial RAM image is constrained by the
> low-segment capacity; see section 3.2.5. An initial RAM image that exceeds
> available low-segment physical memory will overflow into the high segment,
> invalidating any compiled logical addresses in the overflow region.

### 4.2 Memory Regions at Runtime

At startup, the VM address space is organized into four regions.

The **initial RAM image** occupies the low RAM segment beginning at logical
address `0x0000`. Its size in words is given by the image header field at byte
offset `0x16` (see section 3.2.1). The initial RAM image is populated from the
`.MME` file before execution begins; it holds the initial values of program
globals and any statically allocated aggregates.

The **low vector-heap space** is the portion of the low RAM segment at addresses
above the initial RAM image. It is managed by the vector allocator.

The **high vector-heap space** is the lower portion of the high RAM segment,
beginning at logical address `0x8000`. It is also managed by the vector
allocator.

The **tuple stack** occupies the uppermost 1/64th of the high RAM segment; if
a full 64 KiB is available in the high RAM segment, the tuple stack occupies 512
words at logical addresses `0xFE00–0xFFFF`. The tuple stack grows downward from
its upper bound toward lower addresses as tuples are allocated.

> **Note:** The low vector-heap space and the high vector-heap space are
> managed independently. There is no guarantee that they are physically
> contiguous; see section 4.1 regarding the possible address gap between
> segments.

The interpreter maintains bookkeeping for the vector allocator and the
tuple-stack pointer. This bookkeeping is implementation-defined. The
interpreter may store it in VM-visible memory within either vector-heap space
or within any freed or unallocated region of those areas, at addresses above
the end of the initial RAM image. The program must not read or rely on the
values stored in any location used for interpreter bookkeeping.

### 4.3 Vector Allocator

The **vector heap** consists of the low vector-heap space and the high
vector-heap space described in section 4.2. The two spaces are managed
independently; a single vector allocation is served entirely from one segment
and shall not straddle the boundary between segments.

All allocations are in whole words.

`VALLOC` allocates a vector of `$1` total words from the vector heap and pushes
the handle of its first word.

`VALLOCI` allocates a vector of `$1` total words, fills it with initializer
values popped from the evaluation stack in original push order (the first value
pushed is stored at the first word of the vector), and pushes the handle.

`VFREE` returns a previously allocated vector to the allocator. It requires the
handle of the vector (`$2`) and the total word count used when it was allocated
(`$1`). The program must supply the same word count to `VFREE` that it
originally passed to `VALLOC` or `VALLOCI`; the interpreter may validate this
constraint.

The precise implementation of the allocator is not specified. The interpreter
may use VM-visible memory within the vector-heap spaces or within freed or
unallocated memory for allocator state.

### 4.4 Tuple Stack

The **tuple stack** is the 512-word region at logical addresses `0xFE00–0xFFFF`
in the high RAM segment (see section 4.2). Its size is 1/64 of the 32,768-word
high RAM segment. The interpreter maintains an implementation-internal
**tuple-stack pointer** identifying the current top of the stack. The tuple
stack grows downward: allocating a tuple advances the pointer toward lower
addresses; freeing tuple memory advances it toward higher addresses.

All tuple allocations are in whole words.

`TALLOC` allocates a tuple of `$1` total words, advances the tuple-stack
pointer downward by `$1` words, and pushes the handle of the first word of the
newly allocated tuple.

`TALLOCI` allocates a tuple of `$1` total words, fills it with initializer
values popped from the evaluation stack in original push order, and pushes the
handle.

Tuple memory is reclaimed in two ways:

- **Explicit reclamation**: `TPOP` advances the tuple-stack pointer upward by
  `$1` words. The program must supply the exact word count of the tuple being
  freed. `TPOP` does not consume a handle; it simply moves the pointer.
- **Implicit reclamation**: `LONGJMP` and `LONGJMPR` restore the tuple-stack
  pointer to the depth captured by the corresponding `SETJMP`, implicitly
  freeing all tuples allocated since that save point. For full semantics, see
  section 6 (Procedures and Execution).

The current depth of the tuple stack is part of the activation record saved by
`SETJMP` and restored by `LONGJMP` and `LONGJMPR`; see section 6.

> **Note:** The tuple-stack size is 512 words. Programs that exhaust this space
> produce undefined behavior.

### 4.5 Aggregate Handles and Visible Layout

An **aggregate** is a contiguous range of words in VM RAM accessed through a
**handle**. Aggregates may be any of three kinds:

- A static array with a fixed logical address inside the initial RAM image.
- A **vector**, allocated dynamically from the vector heap (see section 4.3).
- A **tuple**, allocated dynamically from the tuple stack (see section 4.4).

All three kinds are accessed through the same opcode families and obey the same
handle and indexing conventions described in this section.

#### 4.5.1 Length Words

The first word of an aggregate, at physical word offset 0, may serve as a
**length word**. When used as a length word, it holds a count of the accessible
elements following it. The VM does not enforce the presence or absence of a
length word; whether a given aggregate carries one is a convention applied by
the program and by specific opcodes.

#### 4.5.2 Access Conventions

Opcodes that access aggregates use one of three indexing conventions. Each
opcode's entry in Appendix B identifies which convention it applies.

**Convention A — one-based word access, no length-word skip.** The index counts
words from the start of the aggregate, beginning at 1. Index 1 refers to the
word at physical word offset 0 (the first word, which may be a length word).
Index 2 refers to the word at offset 1, and so on. There is no implicit skip.

**Convention B — one-based access with length-word prefix.** The aggregate is
expected to carry a length word at physical word offset 0. Indexing begins at 1
and counts from the word immediately following the length word: index 1 refers
to the word at physical word offset 1. For byte access under Convention B, the
byte stream begins at the first byte of the word at physical offset 1; byte
position 1 is the first byte of that word, byte position 2 is its second byte,
and so on.

**Convention C — zero-based physical-offset access.** Offset 0 refers to the
first word of the aggregate (at physical word offset 0). For word access, each
increment of the offset advances by one word. For byte access, each increment
advances by one byte.

> **Note:** Convention B corresponds to the **visible index** convention:
> visible index 1 refers to the word at physical offset 1, immediately
> following the length word. Convention C is used by the inline-operand opcode
> variants (the opcodes whose mnemonics end with an underscore). Appendix B
> entries for each opcode state which convention the opcode uses.

### 4.6 Strings

A **string** is an aggregate in VM RAM with the following layout:

1. First word (physical word offset 0): the count of the string's content bytes.
2. Bytes beginning at the first byte of physical word offset 1: the string
   content, in order.

Strings are the standard representation for text passed to display, comparison,
and string-manipulation opcodes. The handle of a string's first word is referred
to as a **string handle**.

String opcodes that use Convention B (section 4.5.2) treat string content as a
byte stream beginning immediately after the length word; the length-word value
gives the number of valid bytes.

### 4.7 Floating-Point Representation

Floating-point values are encoded as 64-bit IEEE 754 double-precision numbers
in **big-endian byte order**. This byte order applies regardless of the byte
order used for integer words elsewhere in the architecture.

Floating-point values are stored in VM RAM. Operands to floating-point opcodes
are the handles of the aggregates holding those values; the program is
responsible for allocating the required memory before invoking a floating-point
opcode. The program is likewise responsible for allocating destination memory
before invoking any floating-point opcode that writes a result.

The notation `real($n)` used in Appendix B opcode entries denotes the
floating-point value stored in VM RAM at handle `$n`, interpreted as defined by
this section.

> **Note:** On little-endian host systems, the interpreter must reverse the byte
> order of floating-point values when reading them from VM RAM and when writing
> results back to VM RAM. This byte-reversal applies to the full 8-byte double;
> it does not apply to integer words.

## 5. Instruction Encoding

The instruction stream of the Cornerstone VM is encoded in one of three forms:
base opcodes (`0x00–0x5E`), extended opcodes (`0x5F xx`), and packed one-byte
families (`0x60–0xFF`). All instruction bytes are organized into contiguous
256-byte code blocks. Sections 5.2 through 5.4 define the block model, all
inline operand encoding rules, and the packed family bit-field layouts.

### 5.1 Opcode Space

The one-byte opcode space is partitioned into four regions:

| Range | Form |
|-------|------|
| `0x00–0x5E` | Base opcodes |
| `0x5F` | Extended-opcode prefix |
| `0x60–0x9F` | Packed `VREAD_loc_vec` family |
| `0xA0–0xBF` | Packed `PUSH_Ln` family |
| `0xC0–0xDF` | Packed `PUT_Ln` family |
| `0xE0–0xFF` | Packed `STORE_Ln` family |

This partition is sufficient to determine instruction form, mnemonic family,
and operand length from the first byte alone.

A **base opcode** is a single byte in the range `0x00–0x5E`. The interpreter
fetches and dispatches it directly without a prefix byte. The byte `0x5F` is
reserved as the extended-opcode prefix and is not itself a base opcode.

An **extended opcode** consists of the prefix byte `0x5F` followed by a
one-byte secondary opcode byte. The defined secondary range is `0x00–0x38`.

The range `0x60–0xFF` is occupied by four **packed opcode families**. In a
packed instruction, the operand is encoded within the single opcode byte
itself; no inline operand bytes follow. The four families are defined in
section 5.4.

### 5.2 Code Block Structure

The instruction stream is divided into 256-byte aligned units called **code
blocks**. Every code block must be terminated by a `NEXTB` instruction. The
bytes between `NEXTB` and the end of the 256-byte block are padding; the block
shall contain sufficient padding bytes so that the following code block begins
on a 256-byte boundary.

A procedure may span any number of code-block boundaries. No instruction and
no procedure header shall straddle a code-block boundary; each must lie
entirely within a single code block.

> **Note:** The procedure header structure, including variable-length
> initializer records, is defined in section 6.1. The boundary constraint
> applies to the full procedure header, including any initializer records
> that follow the first byte.

The `NEXTB` instruction has no inline operand bytes. Upon executing `NEXTB`,
the interpreter shall advance the program counter to the start of the following
code block. Padding bytes between `NEXTB` and that boundary shall not be
decoded as instructions.

### 5.3 Inline Operand Encoding

Base opcodes and extended opcodes may be followed by zero or more inline
operand bytes. For base opcodes, inline bytes immediately follow the opcode
byte. For extended opcodes, inline bytes follow the secondary opcode byte. The
number and kind of inline bytes depend on the opcode; packed opcodes
(`0x60–0xFF`) carry no inline bytes.

#### 5.3.1 General Rules

All inline words are little-endian. The number and size of inline operands for
each opcode are determined by the operand-class tables in sections 5.3.3 and
5.3.4.

#### 5.3.2 Jump Target Encoding

Jump opcodes (`0x30–0x3E`) use a variable-length inline operand. Two forms are
defined:

1. **Short form.** If the first inline byte is nonzero, it is a signed 8-bit
   offset. The branch target is the module-relative byte offset of the first
   byte of the next instruction, plus this signed value.
2. **Long form.** If the first inline byte is `0x00`, it is followed by a
   little-endian word giving the module-relative byte offset of the branch
   target directly. The total inline operand length in the long form is three
   bytes.

> **Note:** The long form accommodates branch targets that lie outside the
> signed 8-bit range reachable by the short form.

#### 5.3.3 Base-Opcode Operand Classes

The following table defines the inline operand encoding for base opcodes.
Base opcodes not listed in any row have no inline operands.

| Inline operand | Opcodes |
|----------------|---------|
| One unsigned byte | `INCL`, `DECL`, `VLOADW_`, `VLOADB_`, `VPUTW_`, `VPUTB_`, `LOADG`, `LOADMG`, `PUSHB`, `PUTMG`, `INCLV`, `PUTL`, `PUSHL`, `STOREL` |
| One little-endian word | `PUSHW`, `HALT`, `BITSVL`, `BITSV`, `BBSETVL`, `BBSETV`, `BSETVL`, `BSETV` |
| Jump target (see section 5.3.2) | All jump opcodes `0x30–0x3E` |
| Procedure offset — one little-endian word (see section 6.3) | `CALL0`–`CALL3` |
| One unsigned byte (argument count) | `CALL`, `CALLF` |
| Procedure selector — one little-endian word (see section 6.3) | `CALLF0`–`CALLF3` |

> **Note:** `NEXTB` has no inline operands and does not appear in this
> table. Its role as the mandatory code-block terminator is defined in
> section 5.2.

#### 5.3.4 Extended-Opcode Operand Classes

After the `0x5F` prefix byte and the secondary opcode byte, extended opcodes
may carry inline operands as specified below. Extended opcodes not listed in
any row have no inline operands.

| Inline operand | Opcodes |
|----------------|---------|
| One unsigned byte | `PUTG`, `OPEN`, `DISP`, `XDISP`, `DECMG`, `DECG`, `POPI`, `INCG`, `INCMG`, `STOREMG`, `STOREG`, `RETN` |
| Two module-relative byte offsets | `SETJMP` |

For the semantics of `SETJMP`'s two inline offsets, see section 6.7.

### 5.4 Packed One-Byte Families

The four packed opcode families occupy the range `0x60–0xFF`. Each packed
instruction is a single byte; its operand is derived from bit fields within
that byte, and no inline operand bytes follow.

#### 5.4.1 `VREAD_loc_vec` (opcodes `0x60–0x9F`)

An opcode in this family reads a word from an aggregate and pushes it onto the
evaluation stack. Two fields within the opcode byte identify the source:

- **Bits `0–3`:** the index (0–15) of the local variable that holds the
  aggregate handle.
- **Bits `4–5`:** one of the four **visible index** positions (1–4) within the
  aggregate to read from.

The visible index convention, including the length-word skipping rule, is
defined in section 4.5.

> **Note:** This subsection is the canonical definition of the `VREAD_loc_vec`
> bit-field layout. Appendix A.9 should be read in conjunction with this
> subsection.

#### 5.4.2 `PUSH_Ln` (opcodes `0xA0–0xBF`)

An opcode in this family pushes the value of a local variable onto the
evaluation stack. Bits `0–4` of the opcode byte encode the local variable
index (0–31).

#### 5.4.3 `PUT_Ln` (opcodes `0xC0–0xDF`)

Each opcode in this family selects a local variable by index. Bits `0–4` of
the opcode byte encode the local variable index (0–31).

#### 5.4.4 `STORE_Ln` (opcodes `0xE0–0xFF`)

Each opcode in this family selects a local variable by index. Bits `0–4` of
the opcode byte encode the local variable index (0–31).

## 6. Procedures and Execution

This chapter defines the complete procedure lifecycle: the binary format of the
procedure header, the placement rules for procedures within a module, the two
call families and the procedure selector format, argument passing and local
variable initialization, the call-frame model, all return forms, non-local
control transfer, and program termination.

### 6.1 Procedure Header Format

Every procedure begins with a **procedure header**. The first byte of the
header is interpreted as follows:

| Bits | Meaning |
|------|---------|
| `0–6` | Local variable count (0–127) |
| `7` | Initializer-present flag |

A procedure may declare at most 127 local variables.

If bit 7 of the first header byte is set, one or more **initializer records**
follow immediately. Each initializer record consists of an introduction byte
followed by one or two value bytes, as described below.

**Introduction byte:**

| Bits | Meaning |
|------|---------|
| `0–5` | Local variable index (0–63) |
| `6` | Value-size flag |
| `7` | Final-record flag |

If the value-size flag (bit 6) is clear, the introduction byte is followed by
a little-endian word giving the initial value. If bit 6 is set, the
introduction byte is followed by a single byte; the interpreter shall
sign-extend that byte to a word before storing it in the named local variable.

Only local variables with indices 0–63 may name themselves in an initializer
record. Local variables with indices 64–127 are initialized to FALSE unless
supplied with a value by the caller (see section 6.4).

The interpreter shall process initializer records sequentially until it
encounters one with bit 7 set; that record is the last initializer record for
the procedure.

The combined size of all initializer records for a single procedure must not
exceed 255 bytes.

> **Provisional:** The rationale for the 255-byte combined initializer-record
> size limit has not been confirmed against MME.

### 6.2 Procedure Boundaries

A procedure may start at any byte offset within a module. Procedure headers
must not straddle a code-block boundary; see section 5.2 for the code-block
model and the full boundary constraint.

The length of a procedure is not stated in the image format. An exported
procedure may be assumed to end at the offset of the next higher entry in the
module's export table, or at the end of the module if no higher-offset export
exists. Padding bytes may be present between successive procedures.

### 6.3 Call Forms and Procedure Selectors

A **procedure selector** is a 16-bit value that identifies a procedure in any
module by encoding two indices:

- **high byte**: 1-based module number;
- **low byte**: zero-based export-table index within that module.

> **Note:** A compact statement of the procedure selector format also appears
> in section 3.2.1, provided for the benefit of readers parsing the `.MME`
> image header before reaching this chapter. Section 6.3 is the normative home
> of this definition; section 3.2.1 cross-references it.

The instruction set defines two call families:

1. **Near calls**: `CALL0`, `CALL1`, `CALL2`, `CALL3`, and `CALL`. A near call
   remains within the current module and targets a **procedure offset** encoded
   as an unsigned 16-bit little-endian word.
2. **Far calls**: `CALLF0`, `CALLF1`, `CALLF2`, `CALLF3`, and `CALLF`. A far
   call resolves a **procedure selector** and may transfer control to any module.

Within each family, two forms are available:

- **Fixed-arity form** (`CALL0`–`CALL3`, `CALLF0`–`CALLF3`): the argument
  count is encoded in the opcode; the procedure offset (near call) or procedure
  selector (far call) is supplied as an inline word operand.
- **Stack form** (`CALL`, `CALLF`): the argument count is supplied as an
  unsigned inline byte operand; the procedure offset or selector is taken from
  the top of the evaluation stack.

For stack-form calls, the caller must push arguments first, then push the
procedure offset or selector on top of the evaluation stack. See Appendix B
for the full operand pop sequence of `CALL` and `CALLF`.

### 6.4 Argument Passing and Local Initialization

The caller pushes zero or more arguments onto the evaluation stack before
executing a call instruction. After the call transfers control, the callee's
local variables are initialized as follows:

1. Arguments are popped from the evaluation stack and stored in local
   variables: the last argument pushed (top of the evaluation stack) is stored
   in local `n − 1`, and the first argument pushed (deepest argument on the
   stack) is stored in local `0`, where `n` is the argument count.
2. Local variables not supplied by caller arguments are initialized to the
   values specified by the procedure header's initializer records. A local
   variable not named in any initializer record is initialized to FALSE
   (`0x8001`).

Initializer records in the procedure header shall not overwrite the values of
locals already set by caller arguments.

The entry procedure is called with zero arguments. All of its local variables
are initialized solely from the procedure header's initializer records, or to
FALSE if not named in any record.

### 6.5 Call Frames

A call instruction creates a new **call frame** for the invoked procedure and
preserves the **return context** needed to resume the caller. The call frame
holds the callee's local variables and persists until the procedure exits by a
return instruction or by a non-local control transfer instruction.

The layout of call frames is implementation-defined. Call frames are not
VM-visible.

### 6.6 Return Forms

An ordinary return transfers control to the caller's return address and makes
the returned value or values available on the caller's evaluation stack.

The return opcodes define the following result forms:

| Opcode | Result |
|--------|--------|
| `RETURN` | Pops one word from the evaluation stack and returns it to the caller. |
| `RFALSE` | Returns the value `0x8001` (FALSE) to the caller. |
| `RZERO` | Returns the value `0` to the caller. |
| `RET` | Returns to the caller without returning a value. |
| `RETN` | Returns a count of words to the caller. The count is given by an unsigned inline byte operand. The words are taken from the top of the evaluation stack in order; the word at the callee's top of stack becomes the word at the caller's top of stack after the return. |
| `POPRET` | Discards words from the evaluation stack as described below. |

`POPRET` discards from the evaluation stack the number of words returned by the
most recently executed return instruction. The interpreter maintains a global
word-count register that is set whenever any return instruction executes in any
procedure. `POPRET` reads this register to determine how many words to discard.

> **Note:** The `POPRET` count register is global and is overwritten by any
> return instruction in any procedure. If a call to another procedure occurs
> after a return instruction and before the corresponding `POPRET`, the callee's
> own return instruction will overwrite the register. The program must execute
> `POPRET` before invoking any further procedures if it wishes to discard the
> original return value.

The effect of executing `POPRET` when no return instruction has yet executed in
the current invocation of the VM is undefined.

The effect of executing `POPRET` more than once between successive return
instructions is undefined.

### 6.7 Non-Local Control Transfer

The non-local control-transfer instructions are `SETJMP`, `LONGJMP`, and
`LONGJMPR`.

`SETJMP` takes two **procedure offsets** as inline operands:

- the **void return address**: the module-relative byte offset to which
  execution resumes when `LONGJMP` is invoked with the resulting activation
  token;
- the **value return address**: the module-relative byte offset to which
  execution resumes when `LONGJMPR` is invoked with the resulting activation
  token.

`SETJMP` saves an **activation record** containing the following state:

1. the current depth of the tuple stack (see section 4.4);
2. the current depth of the evaluation stack;
3. the void return address and the value return address;
4. the current module context; and
5. the current call frame (the frame pointer; not the contents of the local
   variables).

The activation record is not VM-visible. `SETJMP` pushes an **activation
token** onto the evaluation stack. The activation token is valid for use with
`LONGJMP` or `LONGJMPR` until the procedure containing the `SETJMP`
instruction returns.

`LONGJMP` shall:

1. consume the activation token from the evaluation stack;
2. restore the tuple stack to the saved depth;
3. restore the evaluation stack to the saved depth;
4. restore the saved module context;
5. restore the saved call frame; and
6. set the program counter to the void return address and resume execution.

`LONGJMPR` shall:

1. consume the activation token from the evaluation stack;
2. preserve the word at the top of the evaluation stack as a return value;
3. restore the tuple stack to the saved depth;
4. restore the evaluation stack to the saved depth;
5. restore the saved module context;
6. restore the saved call frame;
7. set the program counter to the value return address; and
8. push the preserved return value onto the evaluation stack before resuming
   execution.

### 6.8 Process Termination

`HALT` is the only valid method for terminating the program. The program must
not return from the entry procedure; doing so produces undefined behavior.

`HALT 1` denotes normal termination. After terminating the program, the
interpreter may clear the screen.

Any other `HALT` value denotes abnormal termination. After terminating the
program, the interpreter should report the `HALT` value to the user as an
error code.

## 7. Storage Spaces

### 7.1 Storage Class Overview

Apart from the evaluation stack, the data directly accessible to the program
falls into five storage classes:

1. **local variables** — private to the active call frame; accessed via the
   packed `PUSH_Ln`, `PUT_Ln`, and `STORE_Ln` families (opcodes `0xA0–0xFF`);
   see §7.2.
2. **program globals** — shared across all modules; accessed via `LOADG`,
   `PUTG`, `INCG`, and `DECG`; see §7.3.
3. **module globals** — private to the accessing module; accessed via
   `LOADMG`, `PUTMG`, `INCMG`, `DECMG`, and `STOREMG` at indices
   `0x00–0xBF`; see §7.4.
4. **system variables** — interpreter-controlled slots at module-global indices
   `0xC0–0xFF`; accessed via `LOADMG` and `PUTMG`; see §7.5.
5. **RAM** — the word-addressed logical address space; accessed via aggregate
   opcodes; see §4.

Each storage class has its own independent numbering space. Local variable 0,
program global 0, module global at index `0x00`, and logical address `0x0000`
are four distinct locations.

### 7.2 Local Variables

A **local variable** is a word-sized variable private to one call frame. Local
variables do not outlive their call frame; they are created when a procedure is
entered and destroyed when it returns or its frame is unwound by `LONGJMP` or
`LONGJMPR`.

Local variables are numbered from `0`. The first argument passed by the caller
is stored in local variable `0`; subsequent arguments are stored in ascending
order. Any local variable for which no argument was supplied and no initializer
record is present in the procedure header is initialized to **FALSE**
(`0x8001`).

The total number of local variables available in a given procedure is
determined by bits `0–6` of its procedure header (see §6.1) and may range from
`0` to `127`.

> **Note:** The protocol by which the caller's arguments are mapped into the
> callee's local variables, and the role of procedure-header initializer
> records, is described in §6.4. The **FALSE** sentinel is defined in §1.5.1.

### 7.3 Program Globals

**Program globals** are word-sized variables shared across all modules and all
procedures. The program accesses them using `LOADG`, `PUTG`, `INCG`, and
`DECG`.

The number of program globals and their initial values are determined by
metadata in the `.MME` file; see §3.2.3. The maximum number of program globals
is 256.

### 7.4 Module Globals

**Module globals** are word-sized variables with independent values in each
module. The program accesses them using `LOADMG`, `PUTMG`, `INCMG`, `DECMG`,
and `STOREMG`.

Module global indices occupy the range `0x00–0xBF`. Indices `0xC0–0xFF` in the
same numbering space are system variables; see §7.5.

The number of module globals may differ between modules. The initial values and
per-module count are determined by metadata in the `.MME` file. The maximum
number of module globals is 192 per module.

### 7.5 System Variables

**System variables** occupy the 64 module-global indices `0xC0–0xFF`. They are
accessed using `LOADMG` and `PUTMG` at those indices. Unlike module globals,
system variables have the same values regardless of which module is accessing
them.

Some system variables are read-only. The effect of writing to a read-only
system variable is undefined.

Some system variables trigger an immediate effect when written. Where such a
trigger is normative, this section states it explicitly.

Where no system variable is defined for a given index in the range
`0xC0–0xFF`, the effect of reading or writing that index is undefined.

The defined system variables are organized below by functional group.

#### 7.5.1 Display and Cursor Slots

##### `0xC1` — Screen Output Gate

> **Provisional:** When system variable `0xC1` contains **FALSE** (`0x8001`),
> screen output is disabled. This behavior has not been confirmed against MME.

##### `0xC4` — Scroll Area Bottom Row

System variable `0xC4` holds the bottom row limit of the bounded scroll area
used by `XDISP`. See §8.2.5.

##### `0xC5` — Scroll Area Alternate Start Row

System variable `0xC5` holds an alternate start row for bounded scroll
operations. `XDISP 4` and `XDISP 5` use this value instead of the cursor row
as the top of the scroll area. See §8.2.5.

##### `0xC7` — Screen Width

System variable `0xC7` contains the screen width in columns minus one.

##### `0xC8` — Screen Height

System variable `0xC8` contains the screen height in rows minus one.

##### `0xC9` — Cursor Column Backing Word

System variable `0xC9` holds the column component of the cursor backing
position. Writing to `0xC9` is the **commit trigger**: the interpreter shall
update the cursor backing position to the pair formed by the current value of
`0xCA` and the value written to `0xC9`.

> **Note:** To set a new cursor position, the program must write the desired
> row to `0xCA` before writing the desired column to `0xC9`, because the
> commit reads `0xCA` at the time of the write to `0xC9`. See §8.2.1 for the
> relationship between the cursor backing position and the physical cursor.

##### `0xCA` — Cursor Row Backing Word

System variable `0xCA` holds the row component of the cursor backing position.
Writing to `0xCA` alone does not commit the cursor backing position; a
subsequent write to `0xC9` is required to commit both components. See §8.2.1.

##### `0xD3` — Active Window-Descriptor Handle

System variable `0xD3` holds the handle of the most recently activated
**window descriptor**. A successful `SETWIN` call updates `0xD3` to the
descriptor handle supplied as its operand. A failed `SETWIN` call clears
`0xD3` to **FALSE**.

> **Note:** `0xD3` may be read by the program to determine the current window
> context. The structure of the window descriptor vector is defined in §8.4.1.
> The `SETWIN` algorithm is defined in §8.4.3.

##### `0xD5` — Text Attribute Flags

System variable `0xD5` holds the active text attribute flags. The following
bit assignments are defined:

* bit `0`: reverse video is enabled when set;
* bit `3`: bright/bold rendering is enabled when set;
* bit `5`: blink rendering is enabled when set.

Bits other than `0`, `3`, and `5` are not used by the display backend.
`0xD5` may be written directly by the program. A successful `SETWIN` call
also updates `0xD5` by mirroring the low byte of `D[2]` (the attribute word
of the descriptor vector). See §8.2.2 and §8.4.3.

##### `0xDB` — Color Mode and Beep Enable

The following bit assignments are defined for system variable `0xDB`:

* bit `0`: set for the color palette, cleared for the monochrome palette;
* bit `15`: set when the beep facility is enabled.

At startup, the monochrome palette is active (bit `0` cleared). See §8.2.2.

#### 7.5.2 Keyboard and Input Slots

##### `0xC0` — KBINPUT Gate

When system variable `0xC0` is nonzero, `KBINPUT` shall return **FALSE**
immediately without inspecting the input buffer. When `0xC0` is zero,
`KBINPUT` operates normally. See §8.3.1.

##### `0xD4` — Ctrl+Break Flag

System variable `0xD4` reflects the state of the Ctrl+Break key combination.
See §8.3.3.

##### `0xD6` — Lock Key Status

System variable `0xD6` is read-only. It reflects the current state of the
keyboard lock keys. The bit assignments are:

* bit `0`: Num Lock is active when set;
* bit `1`: Caps Lock is active when set.

The effect of writing to `0xD6` is undefined. See §8.3.3.

#### 7.5.3 File and I/O Slots

##### `0xCC` — I/O Record Size

System variable `0xCC` supplies the record size, in bytes, used by `READREC`
and `WRITEREC`. See §8.1.4.

##### `0xD7` — Selected File Extension

System variable `0xD7` encodes a three-character file extension as a packed
word. The encoding is:

```
word = enc(str[0]) << 11 | enc(str[1]) × 45 | enc(str[2])
```

where the character encoding `enc(c)` maps:

* `A–Z` → `0–25`;
* `0–9` → `26–35`;
* `$`, `&`, `#`, `@`, `!`, `%`, `-`, `_`, `/` → `36–44`, respectively.

The default value of `0xD7` corresponds to the extension `DBF`.

##### `0xDA` — `.OBJ` Access Word

System variable `0xDA` encodes the pre-opened `.OBJ` file channel number and
the approximate start of the read-only data region as a single word:

```
word = (channel number of the .OBJ file) | (header_word_0x0E << 5)
```

where `header_word_0x0E` is the word at offset `0x0E` in the `.MME` file
header, which holds the byte offset of the start of the read-only data region
divided by 512 (i.e., right-shifted nine bits from the byte offset). The
channel number occupies the low bits; the start-address component occupies the
upper bits.

> **Note:** See §3.3 for the read-only data region layout and §8.1.1 for the
> channel model. Channel 24 is reserved for the printer and is distinct from
> the `.OBJ` channel.

#### 7.5.4 Date and Time Slots

The following system variables reflect the current date and time as maintained
by the host environment:

| Slot | Contents |
|------|----------|
| `0xCD` | Current month |
| `0xCE` | Current day of the month |
| `0xCF` | Current year |
| `0xD0` | Current hour |
| `0xD1` | Current minute |
| `0xD2` | Current second |

> **Note:** The range, epoch, and encoding (e.g., whether month is 1-based or
> 0-based, whether year is absolute or offset) are not specified by this
> edition.

#### 7.5.5 Descriptor System Slots

##### `0xD8` — Descriptor Root

System variable `0xD8` holds the handle of the root object used by `LOOKUP`
and `EXTRACT`. The root object is a four-word vector `R` with the following
defined fields:

* `R[0]`: handle of the primary lookup table;
* `R[2]`: base handle added to successful relative descriptor values;
* `R[3]`: far-procedure selector for the fallback resolver.

(`R[1]` is not defined by this edition.)

Before invoking `LOOKUP` or `EXTRACT`, the program must ensure `0xD8` contains
a valid handle. See §8.5.1 and §8.5.2 for the `LOOKUP` and `EXTRACT`
algorithms.

> **Note:** System variable `0xD8` was absent from the §7 system-variable
> table in earlier editions; it was documented only in the Appendix B entry
> for `LOOKUP`. This subsection is its canonical definition.

#### 7.5.6 Implementation Statistics Slots

System variables `0xE7–0xEF` are reserved for implementation-defined runtime
statistics counters. The following slot assignments are defined:

| Slot | Statistic |
|------|-----------|
| `0xE7` | `#outc` |
| `0xE8` | `#outs` |
| `0xE9` | `#curpos` |
| `0xEA` | `#disp` |
| `0xEB` | `#xdisp` |
| `0xEC` | `#gets` |
| `0xED` | `#sets` |
| `0xEE` | `#hsets` |
| `0xEF` | `#vsets` |

An interpreter may leave any or all of these slots unimplemented. When a
statistics slot is not implemented, the result of reading or writing that slot
is undefined. An interpreter that implements a statistics slot shall not use
that slot for a purpose other than its designated statistic.

## 8. Host Services

This chapter defines the normative interface between the VM and the host
environment. It covers file and channel I/O, display output, keyboard input,
the window descriptor system, and the data lookup and extraction system.

System variable semantics — the per-slot definitions for `0xC0`, `0xC4`,
`0xC5`, `0xC9`, `0xCA`, `0xCC`, `0xD3`, `0xD4`, `0xD5`, `0xD6`, `0xD8`,
`0xDB`, and related slots — are normatively defined in §7.5. This chapter
describes how those variables are used in context; it does not restate their
definitions.

Appendix B contains the per-opcode operand details for every instruction
mentioned in this chapter. Where this chapter defines an algorithm or
interface contract, Appendix B cross-references it.

---

### 8.1 File and Channel I/O

#### 8.1.1 Channel Model

The VM provides access to files through an integer channel abstraction.
A **channel** is an integer in the range `0–24` that represents one open
file or device. Channels are obtained from `OPEN` and released by `CLOSE`.

There are 25 channels in total. Channel `24` is reserved for the printer.

At program start, one channel is already open. Its channel number is supplied
in the low bits of system variable `0xDA` (see §7.5.3). The program must not
`CLOSE` this channel unless it intends to stop using the pre-opened file.

> **Note:** The pre-opened channel provides access to the `.OBJ` file as a
> sequential byte stream. The program may read from it to access module data.

#### 8.1.2 Channel Lifecycle

`OPEN` opens or creates a file, assigns a channel to it, and pushes the channel
on success, or **FALSE** on failure. Its inline byte operand is the mode byte
(see below). Its two stack inputs are:

* `$2`: a string handle identifying the file or device by name;
* `$1`: an input whose meaning is not defined by this specification.

`CLOSE` closes the specified channel, releasing it for reuse. It pushes a
status word: `0` indicates success.

> **Provisional:** The behavior of `CLOSE` when the channel is invalid or
> already closed has not been confirmed against MME. Implementations should
> push `0` on success; failure behavior is implementation-defined.

##### Mode byte

The mode byte passed as the inline operand to `OPEN` is structured as follows.

Bits `0–1` select the access family:

| `mode & 0x03` | Meaning |
|---|---|
| `0x00` | Open for reading |
| `0x01` | Delete or create the file, then open for writing |
| `0x02` | Open for reading and writing |
| `0x03` | Delete or create the file, then open for reading and writing |

Additional bits control filename resolution:

| Bit | Meaning when set |
|---|---|
| `2` (`0x04`) | Enable search type 1 |
| `4` (`0x10`) | Enable search type 2 |

If neither bit is set, search type 0 is used. If both bits are set,
search type 1 takes precedence over search type 2.

> **Provisional:** The precise distinctions between search types 0, 1, and 2
> are not defined by this specification. The available evidence suggests that
> type 1 provides more exhaustive filename resolution than type 0, and that
> type 2 is intermediate, but this ordering has not been confirmed against MME.
> Implementations that do not reproduce the original DOS search behavior may
> treat all three search types identically.

##### Filename resolution

The interpreter may recognize `PRN` as a special filename representing a
printer, and `LPT` followed by a digit as an alias for `PRN`.

#### 8.1.3 Stream Transfer

`READ` and `WRITE` transfer data between an open channel and an aggregate in
VM RAM.

`READ` reads data from the channel into the aggregate. Its stack inputs are:

* `$3`: destination aggregate handle;
* `$2`: channel;
* `$1`: word count.

`READ` transfers `$1 × 2` bytes from the channel into the aggregate. The
bytes are written into the aggregate's byte payload (starting two bytes past
the aggregate base), two bytes per word, low byte first, in sequential
order. If fewer bytes are available in the channel before end-of-file, the
remainder of the destination payload is filled with zero bytes. `READ` pushes
`0` on a complete transfer and a nonzero value on a short read.

`WRITE` writes data from the aggregate to the channel. Its stack inputs are:

* `$3`: source aggregate handle;
* `$2`: channel;
* `$1`: count N.

`WRITE` reads `N` words from the source aggregate, starting at the first word
(word index 0), and writes the low byte of each word to the channel as one byte.
Exactly `N` bytes are written to the channel, one byte per word from the source.
`WRITE` pushes `0` on success.

> **Note:** The count operand `N` is simultaneously the number of words
> consumed from the source aggregate and the number of bytes written to the
> channel. Each word contributes one byte (its low byte) to the output.

#### 8.1.4 Record Transfer

`READREC` and `WRITEREC` transfer fixed-size records between an open channel
and an aggregate.

`READREC` reads records from the channel. Its stack inputs are:

* `$4`: destination aggregate handle;
* `$3`: channel;
* `$2`: 0-based index of the first record to read;
* `$1`: number of records to transfer.

The interpreter seeks to byte offset `$2 × S` in the channel, where `S` is
the record size in bytes (see below), then transfers `$1 × S` bytes into
the destination aggregate. Bytes are packed two per word in little-endian
order, starting at the first word (word index 0) of the destination aggregate.
`READREC` pushes the number of bytes successfully transferred.

`WRITEREC` writes records to the channel. Its stack inputs are:

* `$4`: source aggregate handle;
* `$3`: channel;
* `$2`: 0-based index of the first record to write;
* `$1`: number of records to transfer.

The interpreter seeks to byte offset `$2 × S` in the channel, then
transfers `$1 × S` bytes from the source aggregate. Words are read starting
at word index 0 of the source aggregate and written as pairs of bytes in
little-endian order. `WRITEREC` pushes the number of bytes successfully
transferred.

##### Record size

The record size `S` is derived from system variable `0xCC` (see §7.5.3) as
follows:

* If `0xCC` is `0`, the record size is 256 bytes.
* If `0xCC` is in the range `1–127`, the record size is `0xCC × 2` bytes
  (that is, `0xCC` is a word count).
* If `0xCC` is greater than `127`, it is a direct byte count.

#### 8.1.5 File Management

`FSIZE` returns the size of the file associated with channel `$1`, expressed
in units of 256 bytes, truncating any partial block.

`UNLINK` deletes the file named by the string handle `$1` and closes any
open channels to that file. It pushes `0` on success or **FALSE** on failure.

`RENAME` renames a file. Its stack inputs are:

* `$2`: string handle for the current file name;
* `$1`: string handle for the new file name.

`RENAME` pushes a status word. The exact success and failure values are not
defined by this specification.

---

### 8.2 Display Services

The display subsystem provides cursor positioning, text attribute control, and
two families of screen control operations. System variable semantics for the
display system are defined in §7.5.1.

#### 8.2.1 Cursor Position and Commit

The interpreter maintains a cursor position through two system variables:
`0xCA` (cursor row) and `0xC9` (cursor column). The program positions the
cursor by writing to these variables. Writing `0xC9` commits the cursor move:
the interpreter moves the physical cursor to the position encoded by the
current values of `0xCA` and `0xC9`.

The program must write `0xCA` before writing `0xC9` if the intended cursor
position changes both row and column. If only the column changes, writing
`0xC9` alone is sufficient.

> **Provisional:** The interpreter does not move the physical cursor immediately
> when a character is printed. The relationship between physical cursor movement
> and the logical cursor position tracked by `0xCA`/`0xC9` has not been fully
> confirmed against MME. In particular, it is not confirmed whether the
> interpreter defers the physical cursor update until a character is emitted or
> `KBINPUT` is called.

> **Note:** The interpreter does not translate CR (`0x0D`) or LF (`0x0A`)
> characters into cursor movement. The program is responsible for advancing
> the cursor to the next line when needed.

#### 8.2.2 Text Attributes and Color

System variable `0xD5` holds the active text attribute byte (see §7.5.1).

Only three bits of `0xD5` affect the visible display attribute in standard
PC BIOS text mode:

| Bit | Effect when set |
|---|---|
| `0` | Reverse video |
| `3` | Bright/bold rendering |
| `5` | Blink rendering |

The attribute may be written directly by the program. It is also updated
automatically by `SETWIN` (see §8.4.3), which copies the low byte of `D[2]`
from the active descriptor into `0xD5`.

System variable `0xDB` controls the display color mode (see §7.5.1). Bit `0`
of `0xDB` is set for color mode and cleared for monochrome mode. The
monochrome palette is active at startup.

#### 8.2.3 Character Output

`PRCHAR` prints a single code-page-437 character at the current cursor position
and advances the cursor one column to the right. If the cursor is already at
the right edge of the screen, it does not advance further. `PRCHAR` pops one
word; it uses only the low byte of that word as the character code. The code
must be in the range `0–255`.

`PRINTV` prints a range of bytes from an aggregate as characters. Its stack
inputs are:

* `$3`: source aggregate handle;
* `$2`: byte count N;
* `$1`: byte offset O within the aggregate's payload.

`PRINTV` reads `N` bytes from the aggregate's payload starting at payload byte
offset `O` (that is, starting at byte `O + 2` from the aggregate's base address).
Each byte in the range `0x20–0x7E` is printed as the corresponding code-page-437
character. Bytes outside that range are printed as a space.

The cursor advances one position per byte printed. `PRINTV` does not check or
update system variable `0xC9` or `0xCA`.

#### 8.2.4 Cursor and Screen Control (`DISP`)

`DISP` performs a simple cursor or screen operation selected by its inline byte
operand. All `DISP` operations act on the current cursor position, update the
system variables `0xC9` and `0xCA` if they move the cursor, and do not return a
value.

| `DISP` operand | Operation |
|---|---|
| `0` | Move cursor right one column |
| `1` | Move cursor left one column |
| `2` | Move cursor down one row |
| `3` | Move cursor up one row |
| `4` | Erase from the current column to the right edge of the active line, then restore the cursor position |
| `5` | Clear from the current row to the bottom of the active display area |

#### 8.2.5 Area Scroll and Clear (`XDISP`)

`XDISP` performs a scrolling or line-drawing operation selected by its inline
byte operand. `XDISP` pops one word `$1` as a line count or character count,
then pushes `$1` back unchanged.

The scroll operations use the following region boundaries:

* **suboperations 0 and 1**: the scroll area spans from the cursor row to the
  row in system variable `0xC4` (see §7.5.1).
* **suboperations 4 and 5**: the scroll area spans from the row in system
  variable `0xC5` to the row in system variable `0xC4` (see §7.5.1).

When `$1` is zero, scroll operations clear the entire scroll area rather than
scrolling it.

| `XDISP` operand | Operation |
|---|---|
| `0` | Scroll area downward by `$1` lines |
| `1` | Scroll area upward by `$1` lines |
| `2` | No effect |
| `3` | No effect |
| `4` | Scroll alternate area (from `0xC5`) downward by `$1` lines |
| `5` | Scroll alternate area (from `0xC5`) upward by `$1` lines |
| `6` | Draw a horizontal line `$1` characters wide starting at the cursor position |

Although operations `2` and `3` have no effect, they still consume a word from
the evaluation stack.

> **Note:** Suboperations `2` and `3` are confirmed no-ops. Programs must not
> rely on any side effects from these suboperations.

#### 8.2.6 Screen Geometry Variables

Several system variables define the display dimensions and scroll boundaries
used by `DISP` and `XDISP`. Their definitions are in §7.5.1:

* `0xC7` — screen width minus one;
* `0xC8` — screen height minus one;
* `0xC4` — bottom row of the current scroll area;
* `0xC5` — top row of the alternate scroll area (used by `XDISP 4` and `5`).

---

### 8.3 Keyboard Services

#### 8.3.1 `KBINPUT` Behavior

`KBINPUT` is a nonblocking keyboard poll. It pushes a key code if a key is
available, or **FALSE** if no key is ready.

If system variable `0xC0` is nonzero, `KBINPUT` ignores any pending input
and always pushes **FALSE** (see §7.5.2 for the complete gate rule).

A blocking keyboard read may be constructed by calling `KBINPUT` in a loop
with `JUMPF`:

```
:loop
    KBINPUT
    JUMPF :loop
    ; key code is now on top of the stack
```

#### 8.3.2 Key Code Encoding

Most keys produce a single key code, which `KBINPUT` pushes as a word. Keys
that have no ASCII equivalent (navigation keys, function keys, and similar
extended keys) are encoded as a two-step sequence: the first `KBINPUT` call
that reads the key pushes `0x1B`, and a subsequent `KBINPUT` call pushes a
second mapped byte. The program must issue two separate `KBINPUT` calls to
consume an extended key.

The defined two-step sequences are:

| Key | First code | Second code |
|---|---|---|
| Escape | `0x1B` | `0x1B` |
| Shift+Tab | `0x1B` | `0x0F` |
| Up arrow | `0x1B` | `0x48` |
| Down arrow | `0x1B` | `0x50` |
| Left arrow | `0x1B` | `0x4B` |
| Right arrow | `0x1B` | `0x4D` |
| Home | `0x1B` | `0x47` |
| End | `0x1B` | `0x4F` |
| Page Up | `0x1B` | `0x49` |
| Page Down | `0x1B` | `0x51` |
| Insert | `0x1B` | `0x52` |
| Delete | `0x1B` | `0x53` |
| F1 | `0x1B` | `0x3B` |
| F2 | `0x1B` | `0x3C` |
| F3 | `0x1B` | `0x3D` |
| F4 | `0x1B` | `0x3E` |
| F5 | `0x1B` | `0x3F` |
| F6 | `0x1B` | `0x40` |
| F7 | `0x1B` | `0x41` |
| F8 | `0x1B` | `0x42` |
| F9 | `0x1B` | `0x43` |
| F10 | `0x1B` | `0x44` |
| F11 | `0x1B` | `0x85` |
| F12 | `0x1B` | `0x86` |

#### 8.3.3 Lock State and Break Flag

System variable `0xD6` exposes keyboard lock state (see §7.5.2). Two bits
are defined:

| Bit of `0xD6` | Meaning when set |
|---|---|
| `0` | Num Lock is engaged |
| `1` | Caps Lock is engaged |

System variable `0xD4` is the Ctrl+Break flag (see §7.5.2). This variable is
`0` when Ctrl+Break has been pressed since the flag was last cleared, and
nonzero otherwise.

> **Provisional:** The full behavior of `0xD4` — in particular, when and how
> it is reset, and whether the program is expected to clear it — has not been
> confirmed against MME.

---

### 8.4 Window Descriptor System

The window descriptor system provides positioned, clipped text rendering.
A **window descriptor** is a vector whose visible indexes encode a logical
cursor position and a reference to a geometry vector that defines the window's
coordinate mapping.

The two opcodes in this system are `SETWIN` (`0x5F 0x22`) and `WPRINTV`
(`0x5F 0x21`). Both consume a window descriptor handle and operate on the
coordinate mapping it encodes. The active descriptor handle is stored in
system variable `0xD3` (see §7.5.5).

#### 8.4.1 Descriptor Vector Layout

A window descriptor `D` is a vector with at least four visible words:

| Visible index | Role |
|---|---|
| `D[0]` | Logical column position (word 0 of the aggregate) |
| `D[1]` | Logical row position |
| `D[2]` | Attribute flags word; low byte is mirrored into `0xD5` by `SETWIN` |
| `D[3]` | Handle of the geometry vector `G` |

> **Note:** Descriptor visible indexes follow the convention defined in §4.5:
> index `1` is the first data word of the aggregate, `2` the second, and so on.
> Here, and in the SETWIN and WPRINTV algorithms below, the shorthand `D[0]` and
> `D[3]` refers to aggregate word indexes 0 and 3 (i.e., visible indexes 1 and
> 4).

#### 8.4.2 Geometry Vector Layout

A geometry vector `G = D[3]` provides the coordinate origin and bounds for
one window:

| Aggregate word index | Role |
|---|---|
| `G[0]` | Logical column origin (left edge of the window in logical space) |
| `G[1]` | Logical row origin (top edge of the window in logical space) |
| `G[2]` | Physical column base (screen column corresponding to `G[0]`) |
| `G[3]` | Physical row base (screen row corresponding to `G[1]`) |
| `G[4]` | Column span (exclusive upper bound: valid logical columns are `G[0]` through `G[0] + G[4] − 1`) |
| `G[5]` | Row span (exclusive upper bound: valid logical rows are `G[1]` through `G[1] + G[5] − 1`) |

#### 8.4.3 `SETWIN` Algorithm

`SETWIN` activates the display window described by descriptor handle `$1`.

On success, `SETWIN` pushes `D[0]` (the logical column).
On failure, `SETWIN` pushes **FALSE** and clears the active-descriptor
system variable (`0xD3` is set to **FALSE**).

`SETWIN` returns **FALSE** immediately if the descriptor handle is **FALSE**
or zero.

Otherwise, `SETWIN` validates and maps coordinates as follows:

1. Let `col_delta = D[0] − G[0]`.
2. Let `row_delta = D[1] − G[1]`.
3. The descriptor is valid if and only if both:
   * `0 ≤ col_delta < G[4]`; and
   * `0 ≤ row_delta < G[5]`.
4. If the descriptor is not valid, set `0xD3` to **FALSE** and return
   **FALSE**.

On success, `SETWIN` shall:

1. compute `physical_col = G[2] + col_delta`;
2. compute `physical_row = G[3] + row_delta`;
3. store the descriptor handle in system variable `0xD3` (see §7.5.5);
4. extract bits `0–7` of `D[2]` (the low byte of the attribute flags word)
   and store them in system variable `0xD5` (see §7.5.1);
5. move the physical cursor to `(physical_col, physical_row)`;
6. return `D[0]`.

#### 8.4.4 `WPRINTV` Algorithm

`WPRINTV` paints a clipped range of bytes from a source string into a display
window. Its stack inputs are:

* `$4 = D`: window descriptor handle;
* `$3 = S`: source aggregate (string) handle;
* `$2 = N`: requested character count;
* `$1 = O`: source payload byte offset.

`WPRINTV` returns the result of the final `SETWIN` call on success, or
**FALSE** on failure.

`WPRINTV` returns **FALSE** immediately if `D` or `S` is **FALSE** or zero,
or if the geometry handle `D[3]` is **FALSE** or zero.

Otherwise, `WPRINTV` shall operate as follows:

1. Read the source byte limit `L = S[0]` (the first word of the source
   aggregate, which is the Pascal length word).
2. Let the requested logical start column be `start = D[0]`.
3. Let the requested logical end column be `end_req = start + N`, clipped
   against the source limit so that `end = min(end_req, start + max(0, L − O))`.
4. Let `win_min = G[0]` and `win_max = G[0] + G[4]`.
5. Apply left clipping: if `start < win_min`, advance `O` by `win_min − start`
   and set `start = win_min`.
6. Apply right clipping: set `end_clipped = min(end, win_max)`.
7. Compute the visible character count `count = max(0, end_clipped − start)`.
   An implementation may cap the count at a maximum of 100 characters per call.
8. Write `start` into `D[0]`.
9. Call `SETWIN(D)`. If `SETWIN` returns **FALSE**, write `end_req` into
   `D[0]` and return **FALSE**.
10. If `count > 0`, print `count` bytes from the source aggregate starting at
    payload byte offset `O`, using the `PRINTV` character-printing rules.
11. Write `end_req` into `D[0]`.
12. Call `SETWIN(D)` again so that the descriptor reflects the logical
    post-print cursor column even when the visible span was right-clipped.
13. Return the result of the second `SETWIN` call.

> **Note:** Step 12 advances the descriptor's logical column to the column
> that follows the last requested character, regardless of clipping.
> This ensures that a caller can iterate `WPRINTV` calls without manually
> tracking how many characters were actually visible.

---

### 8.5 Data Lookup and Extraction

The data lookup and extraction system provides access to a structured
descriptor table. `LOOKUP` resolves a packed two-level key to a descriptor
vector handle. `EXTRACT` traverses and extracts data from a variety of
packed record layouts.

Both operations depend on system variable `0xD8`, which holds the handle of
a root vector `R` that anchors the descriptor table (see §7.5.5).

#### 8.5.1 Field Descriptors

A **field descriptor** is a 16-bit packed key that identifies a specific
descriptor vector in a two-level table. The encoding is:

* **bits 0–6** (low 7 bits): primary 1-based index `I` into the primary table;
* **bits 8–15** (high byte): secondary 1-based index `J` into the secondary
  table selected by `I`.

A field descriptor with `I = 0` or `J = 0` is invalid; passing one to
`LOOKUP` produces undefined behavior.

The root object `R = 0xD8` is a four-word vector:

| Aggregate word index | Role |
|---|---|
| `R[0]` | Handle of the primary lookup table |
| `R[1]` | Not defined by this specification |
| `R[2]` | Base handle added to relative descriptor values |
| `R[3]` | Procedure selector for the fallback resolver |

#### 8.5.2 `LOOKUP`

`LOOKUP` resolves a field descriptor `$1` to a descriptor vector handle.

The lookup process is:

1. Read the primary table handle `P = R[0]`.
2. Read the secondary table handle `S = P[2 × (I − 1)]`, where `I` is the
   primary index extracted from `$1`.
3. If `S` is `0` or **FALSE**, initiate a far call to the fallback resolver
   `R[3]`, passing the original packed key as its sole argument. The return
   value of that call is the result of the `LOOKUP` instruction.
4. Otherwise, read the relative descriptor value `D = S[J − 1] & 0x3FFF`,
   where `J` is the secondary index extracted from `$1`.
5. If `D ≠ 0x3FFF`, push `R[2] + D` as the result.
6. If `D = 0x3FFF`, initiate the same fallback far call to `R[3]`.

#### 8.5.3 `EXTRACT`

`EXTRACT` is a multi-mode descriptor walker and extractor. Given an aggregate
containing packed record data, it selects a field or component from that data,
optionally copies it to a destination aggregate, and pushes the byte length of
the selected item (or **FALSE** on failure).

The eight stack inputs are, from deepest (`$8`) to shallowest (`$1`):

| Stack position | Name | Role |
|---|---|---|
| `$8` | `B` | Base aggregate handle |
| `$7` | `S` | Available span, in words |
| `$6` | `D` | Destination aggregate handle, or **FALSE** for a read-only query |
| `$5` | `A` | Auxiliary selector |
| `$4` | `K` | Auxiliary selector or starting word offset |
| `$3` | `O` | Explicit word offset within the adjusted source, or **FALSE** |
| `$2` | `G` | Nonzero-content guard, or **FALSE** |
| `$1` | `M` | Mode or component selector |

##### Data layouts

`EXTRACT` recognizes three encoded layouts:

An **ordinary record** begins with a header word whose low byte is a tag and
whose high byte is a component count. If the component count is zero, the
record payload is a single packed field. Otherwise, the payload is a sequence
of that many packed fields.

A **packed field** begins with a length byte `L`:

* If bit 7 of `L` is clear, the field contains `L` data bytes following the
  length byte, and its encoded size is `L + 1` bytes rounded up to the next
  whole number of words.
* If bit 7 of `L` is set, the field is a fixed-size 8-byte real value occupying
  4 words.

A **typed field** begins with a header word whose low byte is a kind and
whose high byte is a component count:

* If the header word is `0`, the field is empty and occupies one word.
* If `kind = 0`, the following word gives the total field length in words, and
  the remaining bytes form a variable-size composite payload.
* If `kind > 0`, the field contains `count` fixed-width components, each
  exactly `kind` bytes long; the total field size is
  `1 + ⌈count × kind / 2⌉` words.

##### Step 1: Computing the effective base

1. If `K` is not **FALSE**, treat `K` as an initial word offset into `B` and
   advance past `A` typed fields (using typed-field sizing rules) to obtain
   the effective base `E` and the remaining word span `S' = S − (E − B)`.
2. Otherwise, treat `A` as a raw word offset: `E = B + A`, `S' = S − A`.

##### Step 2: Selecting the target item

Once `E` and `S'` are established:

1. If `O` is not **FALSE**, interpret `E + O` as the start of one ordinary
   record and operate on that record.
2. Otherwise, if `M` is not **FALSE**, first attempt to interpret `E` as the
   start of an ordinary record; if that fails, reinterpret `E` as a typed
   field.
3. Otherwise, if `G` is not **FALSE**, treat `G` raw bytes starting at `E`
   as the selected byte span (raw-byte-span path).
4. Otherwise, treat `E` as starting with a leading packed field whose byte
   count is taken from the low byte of its first word (leading-packed-field
   path).

##### Step 3: Applying the mode

`M` determines the result:

* **`M = FALSE`** — operate on the whole selected item.
* **`M = 0`** or **`M = 0xFFFF`** — return the selected item's high-byte
  component count field.
* **`M = n` where `n > 0`** — select the `n`th component of the selected item.

In whole-item mode:

* In ordinary-record path: the result is the payload length in bytes; if `D`
  is a usable destination of sufficient capacity, only the payload bytes are
  copied there.
* In typed-field path: the result is the payload length in bytes; if `D` is
  large enough, the copy includes the typed field starting at its header.
* In raw-byte-span path: the result is exactly `G` bytes; those bytes are
  copied to `D` if `D` is large enough.
* In leading-packed-field path: the result is the field byte length; that many
  raw bytes are copied from the word immediately following the length word.

In component-selection mode (`M = n > 0`):

* In ordinary-record path: component `n` is the `n`th packed subfield of the
  record payload.
* In typed-field path with `kind = 0`: component `n` is the `n`th packed
  subfield of the composite payload.
* In typed-field path with `kind > 0`: component `n` is the `n`th fixed-width
  component, exactly `kind` bytes long.
* In all component-selection paths, the result is the selected component's byte
  length; the bytes are copied to `D` if `D` is large enough.

##### Guard behavior

The optional guard `G` applies in the whole-item ordinary-record path, the
whole-item typed-field path, and typed-field component extraction. In those
paths, if the selected byte span contains no nonzero byte, `EXTRACT` returns
**FALSE** instead of copying or reporting the span.

##### Failure conditions

`EXTRACT` returns **FALSE** if:

* the base handle `B` is invalid;
* the selected structure would extend past the available span `S` in words;
* the requested component `n` does not exist;
* the leading-packed-field path encounters a field of length zero; or
* a guarded selection (`G` ≠ **FALSE**) finds no nonzero byte in the
  selected span.

---


## Appendix A. Opcode Summary Table

### A.1 General Notes

1. Base opcodes are listed in numeric order.
2. Extended opcodes are written as `0x5F xx`.
3. Packed families are described as opcode ranges. Each packed-family opcode
   encodes the complete instruction in a single byte; there are no following
   inline operand bytes. See §5.4 for the bit-field assignments within each
   family.
4. `$N` denotes the Nth word from the top of the evaluation stack at the time
   the instruction executes: `$1` is the top word (the last word pushed), `$2`
   is the next word down, and so on for `$3`, `$4`, …, `$N`.
5. Floating-point arguments are written as `real($1)`, meaning the 8-byte real
   number whose 4-word representation begins at logical address `$1` (see §4.7
   for the format).
6. In the `Operands` column, `-` means no inline operands, `B` means one
   inline byte, `W` means one inline word, `WW` means two inline words, and
   `J` means a mixed jump-target encoding (see §5.3.2).
7. In the `Pops` and `Pushes` columns, `varies` means the exact count depends
   on an inline operand, a value on the evaluation stack, or the number of
   values returned by a called procedure.

---

### A.2 Base Opcodes `0x00–0x10`

| Opcode | Mnemonic | Operands | Pops | Pushes | Description |
| --- | --- | --- | ---: | ---: | --- |
| 0x00 | [`BREAK`](#0x00-break) | `-` | 0 | 0 | reserved debug-trap or breakpoint operation. Structural decoding is defined; full VM-visible semantics are not defined by this edition. |
| 0x01 | [`ADD`](#0x01-add) | `-` | 2 | 1 | pop two words and push their sum. |
| 0x02 | [`SUB`](#0x02-sub) | `-` | 2 | 1 | push `$2 - $1`. |
| 0x03 | [`MUL`](#0x03-mul) | `-` | 2 | 1 | pop two words and push their product. |
| 0x04 | [`DIV`](#0x04-div) | `-` | 2 | 1 | push `$2 / $1`. |
| 0x05 | [`MOD`](#0x05-mod) | `-` | 2 | 1 | push `$2 mod $1`. |
| 0x06 | [`NEG`](#0x06-neg) | `-` | 1 | 1 | push `-$1`. |
| 0x07 | [`ASHIFT`](#0x07-ashift) | `-` | 2 | 1 | push `$2` arithmetically shifted by `$1` bits; positive counts shift left, negative counts shift right with sign extension. |
| 0x08 | [`INCL`](#0x08-incl) | `B` | 0 | 1 | increment local `$B` and push the new value. |
| 0x09 | [`PUSH8`](#0x09-push8) | `-` | 0 | 1 | push constant `8`. |
| 0x0A | [`PUSH4`](#0x0a-push4) | `-` | 0 | 1 | push constant `4`. |
| 0x0B | [`DECL`](#0x0b-decl) | `B` | 0 | 1 | decrement local `$B` and push the new value. |
| 0x0C | [`PUSHm1`](#0x0c-pushm1) | `-` | 0 | 1 | push constant `0xFFFF`. |
| 0x0D | [`PUSH3`](#0x0d-push3) | `-` | 0 | 1 | push constant `3`. |
| 0x0E | [`AND`](#0x0e-and) | `-` | 2 | 1 | bitwise AND. |
| 0x0F | [`OR`](#0x0f-or) | `-` | 2 | 1 | bitwise OR. |
| 0x10 | [`SHIFT`](#0x10-shift) | `-` | 2 | 1 | push `$2` logically shifted by `$1` bits; positive counts shift left, negative counts shift right with zero extension. |

---

### A.3 Base Opcodes `0x11–0x21`

| Opcode | Mnemonic | Operands | Pops | Pushes | Description |
| --- | --- | --- | ---: | ---: | --- |
| 0x11 | [`VALLOC`](#0x11-valloc) | `-` | 1 | 1 | allocate a vector with `$1` total words and push its handle. |
| 0x12 | [`VALLOCI`](#0x12-valloci) | `-` | varies | 1 | pop `$1 = word count`, then pop that many initializer words and allocate a vector containing them in original push order. |
| 0x13 | [`VFREE`](#0x13-vfree) | `-` | 2 | 0 | free the vector identified by handle `$2`, using `$1` as the size in words. |
| 0x14 | [`TALLOC`](#0x14-talloc) | `-` | 1 | 1 | allocate a tuple with `$1` total words and push its handle. |
| 0x15 | [`TALLOCI`](#0x15-talloci) | `-` | varies | 1 | pop `$1 = word count`, then pop that many initializer words and allocate a tuple containing them in original push order. |
| 0x16 | [`VLOADW`](#0x16-vloadw) | `-` | 2 | 1 | load a word from handle `$2` at 1-based word index `$1`; index 1 addresses the first word of the aggregate (no length-word skip). |
| 0x17 | [`VLOADB`](#0x17-vloadb) | `-` | 2 | 1 | load a byte from handle `$2` using 1-based byte index `$1` skipping a length word. |
| 0x18 | [`VLOADW_`](#0x18-vloadw_) | `B` | 1 | 1 | load a word from handle `$1` using the inline zero-based word offset (no length-word skip). |
| 0x19 | [`VLOADB_`](#0x19-vloadb_) | `B` | 1 | 1 | load a byte from handle `$1` using the inline zero-based byte index and skipping a length word. |
| 0x1A | [`VPUTW`](#0x1a-vputw) | `-` | 3 | 0 | store `$1` into handle `$3` at 1-based word index `$2`; index 1 addresses the first word of the aggregate (no length-word skip). |
| 0x1B | [`VPUTB`](#0x1b-vputb) | `-` | 3 | 0 | store the low byte of `$1` into handle `$3` at 1-based byte index `$2`. |
| 0x1C | [`VPUTW_`](#0x1c-vputw_) | `B` | 2 | 0 | store `$1` into handle `$2` at the inline zero-based word offset. |
| 0x1D | [`VPUTB_`](#0x1d-vputb_) | `B` | 2 | 0 | store the low byte of `$1` into handle `$2` at the inline zero-based byte index. |
| 0x1E | [`VECSETW`](#0x1e-vecsetw) | `-` | 3 | 1 | fill handle `$3` with `$2` words of value `$1` and leave that handle on the evaluation stack. |
| 0x1F | [`VECSETB`](#0x1f-vecsetb) | `-` | 3 | 1 | fill handle `$3` with `$2` bytes of value `low8($1)` and leave that handle on the evaluation stack. |
| 0x20 | [`VECCPYW`](#0x20-veccpyw) | `-` | 3 | 1 | copy `$2` words from source handle `$3` to destination handle `$1` and leave the destination handle on the evaluation stack. |
| 0x21 | [`VECCPYB`](#0x21-veccpyb) | `-` | 5 | 1 | copy `$4` bytes from source handle `$5` at byte offset `$2` to destination handle `$3` at byte offset `$1` and leave the destination handle on the evaluation stack. |

---

### A.4 Base Opcodes `0x22–0x2F`

| Opcode | Mnemonic | Operands | Pops | Pushes | Description |
| --- | --- | --- | ---: | ---: | --- |
| 0x22 | [`LOADG`](#0x22-loadg) | `B` | 0 | 1 | load a program global by byte index. |
| 0x23 | [`LOADMG`](#0x23-loadmg) | `B` | 0 | 1 | load a module global by byte index. |
| 0x24 | [`PUSH2`](#0x24-push2) | `-` | 0 | 1 | push constant `2`. |
| 0x25 | [`PUSHW`](#0x25-pushw) | `W` | 0 | 1 | push an inline little-endian word. |
| 0x26 | [`PUSHB`](#0x26-pushb) | `B` | 0 | 1 | push an inline byte as a word-sized value. |
| 0x27 | [`PUSH_NIL`](#0x27-push_nil) | `-` | 0 | 1 | push **FALSE**. |
| 0x28 | [`PUSH0`](#0x28-push0) | `-` | 0 | 1 | push constant `0`. |
| 0x29 | [`DUP`](#0x29-dup) | `-` | 0 | 1 | duplicate the top word of the evaluation stack. |
| 0x2A | [`PUSHm8`](#0x2A-PUSHm8) | `-` | 0 | 1 | push constant `0xFFF8`. |
| 0x2B | [`PUSH5`](#0x2b-push5) | `-` | 0 | 1 | push constant `5`. |
| 0x2C | [`PUSH1`](#0x2c-push1) | `-` | 0 | 1 | push constant `1`. |
| 0x2D | [`PUTMG`](#0x2d-putmg) | `B` | 1 | 0 | store the top word of the evaluation stack into a module global by byte index. |
| 0x2E | [`PUSHFF`](#0x2e-pushff) | `-` | 0 | 1 | push constant `0x00FF`. |
| 0x2F | [`POP`](#0x2f-pop) | `-` | 1 | 0 | discard the top word of the evaluation stack. |

---

### A.5 Base Opcodes `0x30–0x3E`

| Opcode | Mnemonic | Operands | Pops | Pushes | Description |
| --- | --- | --- | ---: | ---: | --- |
| 0x30 | [`JUMP`](#0x30-jump) | `J` | 0 | 0 | unconditional branch. A nonzero inline byte denotes a signed relative jump; zero denotes an absolute 16-bit target in the following word. |
| 0x31 | [`JUMPZ`](#0x31-jumpz) | `J` | 1 | 0 | jump if `$1 == 0`. |
| 0x32 | [`JUMPNZ`](#0x32-jumpnz) | `J` | 1 | 0 | jump if `$1 != 0`. |
| 0x33 | [`JUMPF`](#0x33-jumpf) | `J` | 1 | 0 | jump if `$1 == FALSE`. |
| 0x34 | [`JUMPNF`](#0x34-jumpnf) | `J` | 1 | 0 | jump if `$1 != FALSE`. |
| 0x35 | [`JUMPGZ`](#0x35-jumpgz) | `J` | 1 | 0 | jump if `$1 > 0`. |
| 0x36 | [`JUMPLEZ`](#0x36-jumplez) | `J` | 1 | 0 | jump if `$1 <= 0`. |
| 0x37 | [`JUMPLZ`](#0x37-jumplz) | `J` | 1 | 0 | jump if `$1 < 0`. |
| 0x38 | [`JUMPGEZ`](#0x38-jumpgez) | `J` | 1 | 0 | jump if `$1 >= 0`. |
| 0x39 | [`JUMPL`](#0x39-jumpl) | `J` | 2 | 0 | jump if `$2 < $1`. |
| 0x3A | [`JUMPLE`](#0x3a-jumple) | `J` | 2 | 0 | jump if `$2 <= $1`. |
| 0x3B | [`JUMPGE`](#0x3b-jumpge) | `J` | 2 | 0 | jump if `$2 >= $1`. |
| 0x3C | [`JUMPG`](#0x3c-jumpg) | `J` | 2 | 0 | jump if `$2 > $1`. |
| 0x3D | [`JUMPEQ`](#0x3d-jumpeq) | `J` | 2 | 0 | jump if `$2 == $1`. |
| 0x3E | [`JUMPNE`](#0x3e-jumpne) | `J` | 2 | 0 | jump if `$2 != $1`. |

---

### A.6 Base Opcodes `0x3F–0x4F`

| Opcode | Mnemonic | Operands | Pops | Pushes | Description |
| --- | --- | --- | ---: | ---: | --- |
| 0x3F | [`CALL0`](#0x3f-call0) | `W` | 0 | varies | near call with zero explicit arguments. |
| 0x40 | [`CALL1`](#0x40-call1) | `W` | 1 | varies | near call with one explicit argument. |
| 0x41 | [`CALL2`](#0x41-call2) | `W` | 2 | varies | near call with two explicit arguments. |
| 0x42 | [`CALL3`](#0x42-call3) | `W` | 3 | varies | near call with three explicit arguments. |
| 0x43 | [`CALL`](#0x43-call) | `B` | varies | varies | computed near call; inline byte gives the argument count. |
| 0x44 | [`CALLF0`](#0x44-callf0) | `W` | 0 | varies | far call with zero explicit arguments. |
| 0x45 | [`CALLF1`](#0x45-callf1) | `W` | 1 | varies | far call with one explicit argument. |
| 0x46 | [`CALLF2`](#0x46-callf2) | `W` | 2 | varies | far call with two explicit arguments. |
| 0x47 | [`CALLF3`](#0x47-callf3) | `W` | 3 | varies | far call with three explicit arguments. |
| 0x48 | [`CALLF`](#0x48-callf) | `B` | varies | varies | computed far call; inline byte gives the argument count. |
| 0x49 | [`RETURN`](#0x49-return) | `-` | 1 | 1 | return `$1` to the caller as a one-word result. |
| 0x4A | [`RFALSE`](#0x4a-rfalse) | `-` | 0 | 1 | return **FALSE**. |
| 0x4B | [`RZERO`](#0x4b-rzero) | `-` | 0 | 1 | return `0`. |
| 0x4C | [`PUSH6`](#0x4c-push6) | `-` | 0 | 1 | push constant `6`. |
| 0x4D | [`HALT`](#0x4d-halt) | `W` | 0 | 0 | terminate execution with an inline word argument. |
| 0x4E | [`NEXTB`](#0x4e-nextb) | `-` | 0 | 0 | advance execution to the next 256-byte code block (see §5.2). |
| 0x4F | [`PUSH7`](#0x4f-push7) | `-` | 0 | 1 | push constant `7`. |

---

### A.7 Base Opcodes `0x50–0x5E`

| Opcode | Mnemonic | Operands | Pops | Pushes | Description |
| --- | --- | --- | ---: | ---: | --- |
| 0x50 | [`PRINTV`](#0x50-printv) | `-` | 3 | 0 | print bytes from a VM vector or address range. Broad output role is defined; exact addressing convention is reserved by this edition. |
| 0x51 | [`LOADVB2`](#0x51-loadvb2) | `-` | 2 | 1 | load a raw byte from handle `$2` using alternate 1-based byte index `$1`. |
| 0x52 | [`PUTVB2`](#0x52-putvb2) | `-` | 3 | 0 | store the low byte of `$1` into handle `$3` using the alternate 1-based byte index `$2`. |
| 0x53 | [`REST`](#0x53-rest) | `-` | 2 | 1 | alias encoding of `ADD`. |
| 0x54 | [`INCLV`](#0x54-inclv) | `B` | 0 | 0 | increment a local selected by the inline byte operand without pushing the new value; the local's value must not be **FALSE** (see §1.5.1). |
| 0x55 | [`RET`](#0x55-ret) | `-` | 0 | 0 | return no pushed result. |
| 0x56 | [`PUTL`](#0x56-putl) | `B` | 1 | 0 | store the top word of the evaluation stack into local `$B`. |
| 0x57 | [`PUSHL`](#0x57-pushl) | `B` | 0 | 1 | push local `$B`. |
| 0x58 | [`STOREL`](#0x58-storel) | `B` | 0 | 0 | copy the top word of the evaluation stack into local `$B` without consuming it. |
| 0x59 | [`BITSVL`](#0x59-bitsvl) | `W` | 0 | 1 | extract a bitfield from an aggregate word using an inline control word and an aggregate handle held in a local. |
| 0x5A | [`BITSV`](#0x5a-bitsv) | `W` | 1 | 1 | extract a bitfield from an aggregate word using an inline control word and an aggregate handle from the evaluation stack. |
| 0x5B | [`BBSETVL`](#0x5b-bbsetvl) | `W` | 1 | 0 | replace a multi-bit field inside an aggregate word using a local-held aggregate handle. |
| 0x5C | [`BBSETV`](#0x5c-bbsetv) | `W` | 2 | 0 | replace a multi-bit field inside an aggregate word using an aggregate handle from the evaluation stack. |
| 0x5D | [`BSETVL`](#0x5d-bsetvl) | `W` | 0 | 0 | set or clear one bit in an aggregate word using a local-held aggregate handle. |
| 0x5E | [`BSETV`](#0x5e-bsetv) | `W` | 1 | 0 | set or clear one bit in an aggregate word using an aggregate handle from the evaluation stack. |

---

### A.8 Extended Opcodes `0x5F xx`

| Opcode | Mnemonic | Operands | Pops | Pushes | Description |
| --- | --- | --- | ---: | ---: | --- |
| 0x5F 0x01 | [`XOR`](#0x5f-0x01-xor) | `-` | 2 | 1 | bitwise XOR. |
| 0x5F 0x02 | [`NOT`](#0x5f-0x02-not) | `-` | 1 | 1 | bitwise complement. |
| 0x5F 0x03 | [`ROTATE`](#0x5f-0x03-rotate) | `-` | 2 | 1 | rotate `$2` by `$1` bit positions and push the rotated word. |
| 0x5F 0x04 | [`VFIND`](#0x5f-0x04-vfind) | `-` | 3 | 1 | vector search operation returning an index or **FALSE**. |
| 0x5F 0x05 | [`STRCHR`](#0x5f-0x05-strchr) | `-` | 2 | 1 | search string handle `$1` for character `low8($2)` and push the found index or **FALSE**. |
| 0x5F 0x06 | [`PUTG`](#0x5f-0x06-putg) | `B` | 1 | 0 | store into a program global by byte index. |
| 0x5F 0x07 | [`POPN`](#0x5f-0x07-popn) | `-` | varies | 0 | discard a variable number of words from the evaluation stack. |
| 0x5F 0x09 | [`LONGJMPR`](#0x5f-0x09-longjmpr) | `-` | 2 | 1 | non-local jump through **activation token** `$2`, restoring execution state and pushing `$1` as the return value (see §6.7). |
| 0x5F 0x0A | [`LONGJMP`](#0x5f-0x0a-longjmp) | `-` | 1 | 0 | non-local jump through **activation token** `$1` with no explicit return value (see §6.7). |
| 0x5F 0x0B | [`SETJMP`](#0x5f-0x0b-setjmp) | `WW` | 0 | 1 | save execution state into an **activation record** and push the **activation token**; the two inline words are procedure offsets encoding the resume targets (see §6.7). |
| 0x5F 0x0C | [`OPEN`](#0x5f-0x0c-open) | `B` | 2 | 1 | open or attach a logical channel; the inline byte is the mode bitfield (see §8.1.2). |
| 0x5F 0x0D | [`CLOSE`](#0x5f-0x0d-close) | `-` | 1 | 1 | close a channel and return a status result. |
| 0x5F 0x0E | [`READ`](#0x5f-0x0e-read) | `-` | 3 | 1 | read `$1` words from channel `$2` into aggregate `$3`, beginning at visible index 1 (skipping a length word). |
| 0x5F 0x0F | [`WRITE`](#0x5f-0x0f-write) | `-` | 3 | 1 | write `$1` bytes to channel `$2`; each byte is the low byte of one word from aggregate `$3` beginning at visible index 1, with the high byte of each word discarded. |
| 0x5F 0x10 | [`READREC`](#0x5f-0x10-readrec) | `-` | 4 | 1 | read `$1` records from channel `$3` starting at record `$2` into the byte payload of aggregate `$4` (skipping a length word). |
| 0x5F 0x11 | [`WRITEREC`](#0x5f-0x11-writerec) | `-` | 4 | 1 | write `$1` records from the byte payload of aggregate `$4` to channel `$3` starting at record `$2`. |
| 0x5F 0x12 | [`DISP`](#0x5f-0x12-disp) | `B` | 0 | 0 | display-control suboperation selected by the inline byte (see §8.2.4). |
| 0x5F 0x13 | [`XDISP`](#0x5f-0x13-xdisp) | `B` | 1 | 1 | extended display-control suboperation selected by the inline byte (see §8.2.5). |
| 0x5F 0x14 | [`FSIZE`](#0x5f-0x14-fsize) | `-` | 1 | 1 | file size query in block-like units. |
| 0x5F 0x15 | [`UNLINK`](#0x5f-0x15-unlink) | `-` | 1 | 1 | delete a named file. |
| 0x5F 0x16 | [`POPRET`](#0x5f-0x16-popret) | `-` | varies | 0 | discard the just-returned result bundle. |
| 0x5F 0x17 | [`KBINPUT`](#0x5f-0x17-kbinput) | `-` | 0 | 1 | nonblocking keyboard poll; pushes **FALSE** if no input is ready (see §7.5.2). |
| 0x5F 0x18 | [`FADD`](#0x5f-0x18-fadd) | `-` | 3 | 1 | floating-point addition: `real($2) + real($1)`; result written at `$3` (see §4.7). |
| 0x5F 0x19 | [`FSUB`](#0x5f-0x19-fsub) | `-` | 3 | 1 | floating-point subtraction: `real($2) - real($1)`; result written at `$3` (see §4.7). |
| 0x5F 0x1A | [`FMUL`](#0x5f-0x1a-fmul) | `-` | 3 | 1 | floating-point multiplication: `real($2) × real($1)`; result written at `$3` (see §4.7). |
| 0x5F 0x1B | [`FDIV`](#0x5f-0x1b-fdiv) | `-` | 3 | 1 | floating-point division: `real($2) / real($1)`; result written at `$3` (see §4.7). |
| 0x5F 0x1C | [`TPOP`](#0x5f-0x1c-tpop) | `-` | 1 | 0 | move the tuple-stack pointer upward by `$1` words, freeing that many words of tuple memory (see §4.4). |
| 0x5F 0x1D | [`FLOG`](#0x5f-0x1d-flog) | `-` | 2 | 1 | natural logarithm: `ln(real($1))`; result written at `$2` (see §4.7). |
| 0x5F 0x1E | [`FEXP`](#0x5f-0x1e-fexp) | `-` | 2 | 1 | natural exponential: `e^real($1)`; result written at `$2` (see §4.7). |
| 0x5F 0x1F | [`STRICMP`](#0x5f-0x1f-stricmp) | `-` | 5 | 1 | compare string slice `($4, $2, $1)` against string `($5, 0, $3)` case-insensitively without skipping spaces; push the **signed comparison result** (see §1.5.2). |
| 0x5F 0x20 | [`STRICMP1`](#0x5f-0x20-stricmp1) | `-` | 5 | 1 | as `STRICMP`, but use first string handle `$4 - 1` instead of `$4`. |
| 0x5F 0x21 | [`WPRINTV`](#0x5f-0x21-wprintv) | `-` | 4 | 1 | clipped print into the window described by **window descriptor** handle `$4`; `$3` = source string handle, `$2` = character count, `$1` = source offset (see §8.4.4). |
| 0x5F 0x22 | [`SETWIN`](#0x5f-0x22-setwin) | `-` | 1 | 1 | activate the display window described by **window descriptor** handle `$1` (see §8.4.3). |
| 0x5F 0x23 | [`KEYCMP`](#0x5f-0x23-keycmp) | `-` | 2 | 1 | compare structured sort keys `$1` and `$2` and push the **signed comparison result** (see §1.5.2). |
| 0x5F 0x24 | [`MEMCMP`](#0x5f-0x24-memcmp) | `-` | 3 | 1 | compare `$1` bytes of aggregate `$2` against aggregate `$3` and push the **signed comparison result** (see §1.5.2). |
| 0x5F 0x25 | [`MEMCMPO`](#0x5f-0x25-memcmpo) | `-` | 4 | 1 | compare `$2` bytes of aggregate `$3`, starting at raw byte offset `$1`, against the payload of aggregate `$4`, and push the **signed comparison result** (see §1.5.2). |
| 0x5F 0x26 | [`ADVANCE`](#0x5f-0x26-advance) | `-` | 2 | 1 | derive an offset from an indexed byte in aggregate `$2`; used to advance to the next B-tree node. |
| 0x5F 0x27 | [`DECMG`](#0x5f-0x27-decmg) | `B` | 0 | 1 | decrement a module global and push the new value. |
| 0x5F 0x28 | [`DECG`](#0x5f-0x28-decg) | `B` | 0 | 1 | decrement a program global and push the new value. |
| 0x5F 0x29 | [`POPI`](#0x5f-0x29-popi) | `B` | varies | 0 | discard `$B` words from the evaluation stack. |
| 0x5F 0x2A | [`INCG`](#0x5f-0x2a-incg) | `B` | 0 | 1 | increment a program global and push the new value. |
| 0x5F 0x2B | [`INCMG`](#0x5f-0x2b-incmg) | `B` | 0 | 1 | increment a module global and push the new value. |
| 0x5F 0x2C | [`STOREMG`](#0x5f-0x2c-storemg) | `B` | 0 | 0 | store into a module global while preserving the top of the evaluation stack. |
| 0x5F 0x2D | [`STOREG`](#0x5f-0x2d-storeg) | `B` | 0 | 0 | store into a program global while preserving the top of the evaluation stack. |
| 0x5F 0x2E | [`RETN`](#0x5f-0x2e-retn) | `B` | varies | varies | counted return; inline byte gives the number of returned words. |
| 0x5F 0x2F | [`PRCHAR`](#0x5f-0x2f-prchar) | `-` | 1 | 0 | print one character without character-set translation. |
| 0x5F 0x30 | [`UNPACK`](#0x5f-0x30-unpack) | `-` | 4 | 1 | unpack packed 5-bit text symbols; `$4` = source aggregate, `$3` = destination aggregate, `$2` = word count, `$1` = destination byte offset. |
| 0x5F 0x31 | [`PINM`](#0x5f-0x31-pinm) | `-` | 0 | 0 | preload and pin all code blocks in the current module. |
| 0x5F 0x32 | [`UNPINM`](#0x5f-0x32-unpinm) | `-` | 0 | 0 | unpin the current module's code blocks. |
| 0x5F 0x34 | [`FMTREAL`](#0x5f-0x34-fmtreal) | `-` | 4 | 1 | format an 8-byte real number as a string (see §4.7). |
| 0x5F 0x35 | [`PRSREAL`](#0x5f-0x35-prsreal) | `-` | 6 | 1 | parse a string as an 8-byte real number (see §4.7). |
| 0x5F 0x36 | [`LOOKUP`](#0x5f-0x36-lookup) | `-` | 1 | 1 | resolve **field descriptor** `$1` into a handle (see §8.5.2). |
| 0x5F 0x37 | [`EXTRACT`](#0x5f-0x37-extract) | `-` | 8 | 1 | walk **field descriptor** fields; `$1` is the mode or subfield selector and `$2`–`$8` are the walker parameters (see §8.5.3). |
| 0x5F 0x38 | [`RENAME`](#0x5f-0x38-rename) | `-` | 2 | 1 | rename a file from name `$2` to name `$1`. |

---

### A.9 Packed Opcode Families

| Opcode | Mnemonic | Operands | Pops | Pushes | Description |
| --- | --- | --- | ---: | ---: | --- |
| 0x60–0x9F | [`VREAD_loc_vec`](#0x60-0x9F-VREAD-loc-vec) | `-` | 0 | 1 | packed vector-word read using a local-held handle; see §5.4.1 for the bit assignment. |
| 0xA0–0xBF | [`PUSH_Ln`](#0xA0-0xBF-PUSH-Ln) | `-` | 0 | 1 | packed local push for locals `0`–`31`. |
| 0xC0–0xDF | [`PUT_Ln`](#0xC0-0xDF-PUT-Ln) | `-` | 1 | 0 | packed local store for locals `0`–`31`, consuming the top of the evaluation stack. |
| 0xE0–0xFF | [`STORE_Ln`](#0xE0-0xFF-STORE-Ln) | `-` | 0 | 0 | packed local store for locals `0`–`31`, preserving the top of the evaluation stack. |

## Appendix B. Opcode Reference

This appendix is the authoritative per-opcode definition of the Cornerstone VM
instruction set. Each entry states the opcode byte(s), mnemonic, inline operand
class, net stack effect (words popped / words pushed), and full behavioral
description, including edge cases and implementation constraints.

Where a behavioral rule is canonical in a body chapter, this appendix
cross-references that section and provides only the per-opcode interface
description and any additional operand-level constraints. For the compact
summary table, see Appendix A.

Operand classes used in entries below:

- **B** — one inline byte
- **W** — one inline word (two bytes, little-endian)
- **WW** — two inline words
- **-** — no inline operand bytes

Stack-effect notation: `P/Q` means P words are consumed from the evaluation
stack (top-to-bottom as `$1`, `$2`, …) and Q words are produced. Q = `varies`
means the count depends on a runtime value.

---

### `0x00 BREAK`

Operands: -
Pop/Push: 0/0

Reserved debug-trap or breakpoint slot. The structural decoding of this byte as
a no-operand base opcode is defined. No VM-visible behavior is defined by this
edition of the specification.

> **Provisional:** The intended semantics of `BREAK` (halt, signal debugger, or
> silent no-op) have not been confirmed against MME.

---

### `0x01 ADD`

Operands: -
Pop/Push: 2/1

Integer addition: push `$2 + $1`. The result is truncated to 16 bits; overflow
and underflow result in wrapping.

---

### `0x02 SUB`

Operands: -
Pop/Push: 2/1

Integer subtraction: push `$2 - $1`. The result is truncated to 16 bits;
overflow and underflow result in wrapping.

---

### `0x03 MUL`

Operands: -
Pop/Push: 2/1

Integer multiplication: push `$2 * $1`. The result is truncated to 16 bits;
overflow and underflow result in wrapping.

---

### `0x04 DIV`

Operands: -
Pop/Push: 2/1

Integer division: push `$2 / $1`. The result of dividing by zero is undefined.

---

### `0x05 MOD`

Operands: -
Pop/Push: 2/1

Integer modulus: push `$2 mod $1`.

The result must not be negative. When the dividend or divisor is negative, the
implementation must add `$1` to any negative intermediate result to produce a
non-negative value. Implementations that use the host language's `%` operator
must apply this correction explicitly if the operator has different semantics.

---

### `0x06 NEG`

Operands: -
Pop/Push: 1/1

Integer negation: push `-$1`.

---

### `0x07 ASHIFT`

Operands: -
Pop/Push: 2/1

Arithmetic shift: push `$2` shifted left by `$1` bit positions, or right if
`$1` is negative, extending the sign bit for right shifts.

---

### `0x08 INCL`

Operands: B
Pop/Push: 0/1

Increment local variable `$B` and push the new value.

---

### `0x09 PUSH8`

Operands: -
Pop/Push: 0/1

Push constant `8`.

---

### `0x0A PUSH4`

Operands: -
Pop/Push: 0/1

Push constant `4`.

---

### `0x0B DECL`

Operands: B
Pop/Push: 0/1

Decrement local variable `$B` and push the new value.

---

### `0x0C PUSHm1`

Operands: -
Pop/Push: 0/1

Push constant `0xFFFF` (`-1`).

---

### `0x0D PUSH3`

Operands: -
Pop/Push: 0/1

Push constant `3`.

---

### `0x0E AND`

Operands: -
Pop/Push: 2/1

Bitwise AND: push `$2 & $1`.

---

### `0x0F OR`

Operands: -
Pop/Push: 2/1

Bitwise OR: push `$2 | $1`.

---

### `0x10 SHIFT`

Operands: -
Pop/Push: 2/1

Logical shift: push `$2` shifted left by `$1` bit positions, or right if `$1`
is negative, filling vacated bits with zero.

---

### `0x11 VALLOC`

Operands: -
Pop/Push: 1/1

Allocate a vector of `$1` total words from the vector heap and push its handle.

> **Note:** The argument `$1` is the **total** word count of the allocation:
> the number of words the allocator reserves in the vector heap, including any
> internal header words used by the implementation. The program's responsibility
> is to pass the same total count to `VFREE` when releasing the vector.

The precise operation of the allocator is implementation-defined; see §4.3.

---

### `0x12 VALLOCI`

Operands: -
Pop/Push: varies/1

Pop `$1` as a word count, then pop that many initializer words from the
evaluation stack and allocate a vector containing them in push order: the first
word pushed is stored at word offset 0. Push the new vector handle.

The precise operation of the allocator is implementation-defined; see §4.3.

---

### `0x13 VFREE`

Operands: -
Pop/Push: 2/0

Free the vector identified by handle `$2`, using `$1` as the total word count.
`$2` must have been previously returned by `VALLOC` or `VALLOCI`, and `$1`
must match the total word count used at allocation time.

The precise operation of the allocator is implementation-defined; see §4.3.

---

### `0x14 TALLOC`

Operands: -
Pop/Push: 1/1

Allocate a tuple with `$1` total words from the tuple stack and push its
handle. See §4.4 for the tuple allocator model.

---

### `0x15 TALLOCI`

Operands: -
Pop/Push: varies/1

Pop `$1` as a word count, then pop that many initializer words from the
evaluation stack and allocate a tuple containing them in push order: the first
word pushed is stored at visible index 1. Push the new tuple handle. See §4.4
for the tuple allocator model.

---

### `0x16 VLOADW`

Operands: -
Pop/Push: 2/1

Load a word from handle `$2` at 1-based word index `$1` from the start of
the aggregate. No length-word skip is performed; word index 1 addresses the
first word of the aggregate.

---

### `0x17 VLOADB`

Operands: -
Pop/Push: 2/1

Load a byte from handle `$2` at 1-based byte index `$1 + 2` from the start of
the aggregate. The `+2` accounts for the initial two-byte length word; a value
of `$1 = 1` addresses the first byte of the payload. See §4.5 for the aggregate
layout convention.

---

### `0x18 VLOADW_`

Operands: B
Pop/Push: 1/1

Load a word from handle `$1` at the inline zero-based word offset `$B` from the
start of the aggregate. No length-word skip is performed.

---

### `0x19 VLOADB_`

Operands: B
Pop/Push: 1/1

Load a byte from handle `$1` at the inline zero-based byte offset `$B` from the
start of the aggregate. No length-word skip is performed.

---

### `0x1A VPUTW`

Operands: -
Pop/Push: 3/0

Store `$1` into handle `$3` at zero-based word offset `$2` from the start of
the aggregate. No length-word skip is performed; word offset 0 addresses the
first word of the aggregate.

---

### `0x1B VPUTB`

Operands: -
Pop/Push: 3/0

Store the low byte of `$1` into handle `$3` at 1-based byte index `$2 + 2`
from the start of the aggregate. The `+2` accounts for the initial two-byte
length word; a value of `$2 = 1` addresses the first byte of the payload. See
§4.5 for the aggregate layout convention.

---

### `0x1C VPUTW_`

Operands: B
Pop/Push: 2/0

Store `$1` into handle `$2` at the inline zero-based word offset `$B` from the
start of the aggregate. No length-word skip is performed.

---

### `0x1D VPUTB_`

Operands: B
Pop/Push: 2/0

Store the low byte of `$1` into handle `$2` at the inline zero-based byte
offset `$B` from the start of the aggregate. No length-word skip is performed.

---

### `0x1E VECSETW`

Operands: -
Pop/Push: 3/1

Fill handle `$3` with `$2` copies of the word `$1`, then push `$3`.

> **Provisional:** Whether this instruction skips an initial length word when
> computing the fill region has not been confirmed against MME.

---

### `0x1F VECSETB`

Operands: -
Pop/Push: 3/1

Fill handle `$3` with `$2` copies of the low byte of `$1`, then push `$3`.

> **Provisional:** Whether this instruction skips an initial length word when
> computing the fill region has not been confirmed against MME.

---

### `0x20 VECCPYW`

Operands: -
Pop/Push: 3/1

Copy `$2` words from source handle `$3` to destination handle `$1`, then push
`$1`.

> **Provisional:** Whether this instruction skips an initial length word in
> either the source or destination aggregate has not been confirmed against MME.

---

### `0x21 VECCPYB`

Operands: -
Pop/Push: 5/1

Copy `$4` bytes from source handle `$5` at zero-based byte offset `$2` to
destination handle `$3` at zero-based byte offset `$1`, then push `$3`.

> **Provisional:** Whether an initial length word is skipped in either
> aggregate when computing the base byte offset has not been confirmed against
> MME.

---

### `0x22 LOADG`

Operands: B
Pop/Push: 0/1

Push the value of program global `$B`.

---

### `0x23 LOADMG`

Operands: B
Pop/Push: 0/1

Push the value of module global `$B` (if `$B` is in `0x00–0xBF`) or system
variable `$B` (if `$B` is in `0xC0–0xFF`). See §7.4 and §7.5.

---

### `0x24 PUSH2`

Operands: -
Pop/Push: 0/1

Push constant `2`.

---

### `0x25 PUSHW`

Operands: W
Pop/Push: 0/1

Push the inline word value `$W`.

---

### `0x26 PUSHB`

Operands: B
Pop/Push: 0/1

Push the inline byte value `$B`, zero-extending it to a 16-bit word.

---

### `0x27 PUSH_NIL`

Operands: -
Pop/Push: 0/1

Push **FALSE** (`0x8001`). See §1.5.1.

---

### `0x28 PUSH0`

Operands: -
Pop/Push: 0/1

Push constant `0`.

---

### `0x29 DUP`

Operands: -
Pop/Push: 0/1

Duplicate the top word of the evaluation stack, pushing a second copy.

---

### `0x2A PUSHm8`

Operands: -
Pop/Push: 0/1

Push constant `0xFFF8` (`-8`).

---

### `0x2B PUSH5`

Operands: -
Pop/Push: 0/1

Push constant `5`.

---

### `0x2C PUSH1`

Operands: -
Pop/Push: 0/1

Push constant `1`.

---

### `0x2D PUTMG`

Operands: B
Pop/Push: 1/0

Store `$1` into module global `$B` (if `$B` is in `0x00–0xBF`) or system
variable `$B` (if `$B` is in `0xC0–0xFF`). See §7.4 and §7.5.

---

### `0x2E PUSHFF`

Operands: -
Pop/Push: 0/1

Push constant `0x00FF` (`255`).

---

### `0x2F POP`

Operands: -
Pop/Push: 1/0

Discard the top word of the evaluation stack.

---

### `0x30 JUMP`

Operands: J
Pop/Push: 0/0

Unconditional branch. The jump-target operand uses the variable-length encoding
described in §5.3.2: a nonzero first byte is a signed 8-bit offset relative to
the first byte of the next instruction; a zero first byte is followed by a
little-endian word giving the absolute module-relative target offset.

---

### `0x31 JUMPZ`

Operands: J
Pop/Push: 1/0

Branch if `$1 == 0`. See §5.3.2 for the jump-target encoding.

---

### `0x32 JUMPNZ`

Operands: J
Pop/Push: 1/0

Branch if `$1 != 0`. See §5.3.2 for the jump-target encoding.

---

### `0x33 JUMPF`

Operands: J
Pop/Push: 1/0

Branch if `$1 == FALSE` (`0x8001`). See §5.3.2 for the jump-target encoding.

---

### `0x34 JUMPNF`

Operands: J
Pop/Push: 1/0

Branch if `$1 != FALSE` (`0x8001`). See §5.3.2 for the jump-target encoding.

---

### `0x35 JUMPGZ`

Operands: J
Pop/Push: 1/0

Branch if `$1 > 0`. See §5.3.2 for the jump-target encoding.

---

### `0x36 JUMPLEZ`

Operands: J
Pop/Push: 1/0

Branch if `$1 <= 0`. See §5.3.2 for the jump-target encoding.

---

### `0x37 JUMPLZ`

Operands: J
Pop/Push: 1/0

Branch if `$1 < 0`. See §5.3.2 for the jump-target encoding.

---

### `0x38 JUMPGEZ`

Operands: J
Pop/Push: 1/0

Branch if `$1 >= 0`. See §5.3.2 for the jump-target encoding.

---

### `0x39 JUMPL`

Operands: J
Pop/Push: 2/0

Branch if `$2 < $1`. See §5.3.2 for the jump-target encoding.

---

### `0x3A JUMPLE`

Operands: J
Pop/Push: 2/0

Branch if `$2 <= $1`. See §5.3.2 for the jump-target encoding.

---

### `0x3B JUMPGE`

Operands: J
Pop/Push: 2/0

Branch if `$2 >= $1`. See §5.3.2 for the jump-target encoding.

---

### `0x3C JUMPG`

Operands: J
Pop/Push: 2/0

Branch if `$2 > $1`. See §5.3.2 for the jump-target encoding.

---

### `0x3D JUMPEQ`

Operands: J
Pop/Push: 2/0

Branch if `$2 == $1`. See §5.3.2 for the jump-target encoding.

---

### `0x3E JUMPNE`

Operands: J
Pop/Push: 2/0

Branch if `$2 != $1`. See §5.3.2 for the jump-target encoding.

---

### `0x3F CALL0`

Operands: W
Pop/Push: 0/varies

Near call to the procedure at procedure offset `$W` in the current module,
passing zero arguments. See §6.3.

---

### `0x40 CALL1`

Operands: W
Pop/Push: 1/varies

Near call to the procedure at procedure offset `$W` in the current module,
passing one argument popped from the evaluation stack. See §6.3.

---

### `0x41 CALL2`

Operands: W
Pop/Push: 2/varies

Near call to the procedure at procedure offset `$W` in the current module,
passing two arguments popped from the evaluation stack. See §6.3.

---

### `0x42 CALL3`

Operands: W
Pop/Push: 3/varies

Near call to the procedure at procedure offset `$W` in the current module,
passing three arguments popped from the evaluation stack. See §6.3.

---

### `0x43 CALL`

Operands: B
Pop/Push: varies/varies

Near call to the procedure at the procedure offset given by `$1`, in the
current module, passing `$B` arguments popped from the evaluation stack.
See §6.3.

---

### `0x44 CALLF0`

Operands: W
Pop/Push: 0/varies

Far call to procedure selector `$W`, passing zero arguments. See §6.3.

---

### `0x45 CALLF1`

Operands: W
Pop/Push: 1/varies

Far call to procedure selector `$W`, passing one argument popped from the
evaluation stack. See §6.3.

---

### `0x46 CALLF2`

Operands: W
Pop/Push: 2/varies

Far call to procedure selector `$W`, passing two arguments popped from the
evaluation stack. See §6.3.

---

### `0x47 CALLF3`

Operands: W
Pop/Push: 3/varies

Far call to procedure selector `$W`, passing three arguments popped from the
evaluation stack. See §6.3.

---

### `0x48 CALLF`

Operands: B
Pop/Push: varies/varies

Far call to the procedure selector given by `$1`, passing `$B` arguments
popped from the evaluation stack. See §6.3.

---

### `0x49 RETURN`

Operands: -
Pop/Push: 1/1

Return `$1` to the caller as a one-word result. See §6.6.

---

### `0x4A RFALSE`

Operands: -
Pop/Push: 0/1

Return **FALSE** (`0x8001`) to the caller as a one-word result. See §6.6.

---

### `0x4B RZERO`

Operands: -
Pop/Push: 0/1

Return `0` to the caller as a one-word result. See §6.6.

---

### `0x4C PUSH6`

Operands: -
Pop/Push: 0/1

Push constant `6`.

---

### `0x4D HALT`

Operands: W
Pop/Push: 0/0

Terminate execution with halt code `$W`. A halt code of `1` indicates normal
termination; any other value indicates abnormal termination. See §6.8.

---

### `0x4E NEXTB`

Operands: -
Pop/Push: 0/0

Advance the instruction pointer to the next 256-byte code block. Any bytes
remaining in the current block between the `NEXTB` instruction and the end of
the block are skipped. See §5.2 for the code-block structure and block-boundary
rules.

---

### `0x4F PUSH7`

Operands: -
Pop/Push: 0/1

Push constant `7`.

---

### `0x50 PRINTV`

Operands: -
Pop/Push: 3/0

Print bytes from a vector payload to the display, as if they were printed one at
a time by `PRCHAR`. `$3` is the source vector handle, `$2` is the number of
bytes to print, and `$1` is the byte offset into the payload. An initial length
word is skipped: byte offset `$1 = 0` addresses the first byte after the length
word.

---

### `0x51 LOADVB2`

Operands: -
Pop/Push: 2/1

Load a byte from handle `$2` using 1-based byte index `$1` measured from the
start of the aggregate, with no length-word skip. Byte index `1` addresses the
first byte of the aggregate (i.e., the first byte of the length word in a
string-layout aggregate).

> **Note:** This instruction's indexing convention differs from `VLOADB`, which
> always skips the initial length word. Use `LOADVB2` when direct access to the
> length word or other header bytes is required.

---

### `0x52 PUTVB2`

Operands: -
Pop/Push: 3/0

Store the low byte of `$1` into handle `$3` using 1-based byte index `$2`
measured from the start of the aggregate, with no length-word skip. Byte index
`1` addresses the first byte of the aggregate.

> **Note:** This instruction's indexing convention differs from `VPUTB`, which
> always skips the initial length word.

---

### `0x53 REST`

Operands: -
Pop/Push: 2/1

Push `$2 + $1`.

> **Note:** This instruction is guaranteed by the VM specification to be
> semantically identical to `ADD` (`0x01`). It is documented separately because
> it occupies a distinct opcode position.

---

### `0x54 INCLV`

Operands: B
Pop/Push: 0/0

Increment local variable `$B` in place, without pushing a result.

The value of local `$B` before incrementing must not be **FALSE** (`0x8001`).
See §1.5.1 for the role of **FALSE** as a sentinel; using `INCLV` on an
uninitialized or FALSE-valued local results in undefined behavior.

---

### `0x55 RET`

Operands: -
Pop/Push: 0/0

Return to the caller without producing a result. See §6.6.

---

### `0x56 PUTL`

Operands: B
Pop/Push: 1/0

Pop `$1` from the evaluation stack and store it into local variable `$B`.

---

### `0x57 PUSHL`

Operands: B
Pop/Push: 0/1

Push the current value of local variable `$B`.

---

### `0x58 STOREL`

Operands: B
Pop/Push: 0/0

Copy the top word of the evaluation stack into local variable `$B`, leaving the
stack unchanged.

---

### `0x59 BITSVL`

Operands: W
Pop/Push: 0/1

Extract a bitfield from a word within a vector, using an inline control word
`$W` and a vector handle held in a local variable.

> **Provisional:** The encoding of the inline control word `$W`, the local
> variable index it identifies, and the exact bit-extraction semantics have not
> been confirmed against MME.

---

### `0x5A BITSV`

Operands: W
Pop/Push: 1/1

Extract a bitfield from a word within a vector, using an inline control word
`$W` and a vector handle popped from the evaluation stack.

> **Provisional:** The encoding of the inline control word `$W` and the exact
> bit-extraction semantics have not been confirmed against MME.

---

### `0x5B BBSETVL`

Operands: W
Pop/Push: 1/0

Replace a multi-bit field within a word in a vector, using an inline control
word `$W` and a vector handle held in a local variable. `$1` provides the new
field value.

> **Provisional:** The encoding of the inline control word `$W`, the local
> variable index it identifies, and the exact field-replacement semantics have
> not been confirmed against MME.

---

### `0x5C BBSETV`

Operands: W
Pop/Push: 2/0

Replace a multi-bit field within a word in a vector, using an inline control
word `$W` and a vector handle popped from the evaluation stack. `$1` provides
the new field value and `$2` provides the vector handle.

> **Provisional:** The encoding of the inline control word `$W` and the exact
> field-replacement semantics have not been confirmed against MME.

---

### `0x5D BSETVL`

Operands: W
Pop/Push: 0/0

Set or clear one bit within a word in a vector, using an inline control word
`$W` and a vector handle held in a local variable.

> **Provisional:** The encoding of the inline control word `$W`, the local
> variable index it identifies, and which stack operand (if any) supplies the
> bit value have not been confirmed against MME.

---

### `0x5E BSETV`

Operands: W
Pop/Push: 1/0

Set or clear one bit within a word in a vector, using an inline control word
`$W` and a vector handle popped from the evaluation stack.

> **Provisional:** The encoding of the inline control word `$W` and the exact
> bit-set semantics have not been confirmed against MME.

---

### `0x5F 0x01 XOR`

Operands: -
Pop/Push: 2/1

Bitwise XOR: push `$2 ^ $1`.

---

### `0x5F 0x02 NOT`

Operands: -
Pop/Push: 1/1

Bitwise NOT: push `~$1`.

---

### `0x5F 0x03 ROTATE`

Operands: -
Pop/Push: 2/1

Rotate `$2` right by `$1` bit positions; rotate left if `$1` is negative. Bits
shifted out at one end reappear at the other end in the same order.

---

### `0x5F 0x04 VFIND`

Operands: -
Pop/Push: 3/1

Search a vector for a specific word value. `$1` is the number of words to
search, `$2` is the vector handle, and `$3` is the word value to find. Push
the index of the first matching word, or **FALSE** if no match is found.

> **Provisional:** The index returned is believed to be 1-based, but whether
> an initial length word is skipped when computing the search region and the
> returned index has not been confirmed against MME.

---

### `0x5F 0x05 STRCHR`

Operands: -
Pop/Push: 2/1

Search string handle `$1` for a byte matching the low byte of `$2`. Push the
1-based byte index of the first matching byte within the string payload, or
**FALSE** if no match is found. See §4.6 for the string layout.

---

### `0x5F 0x06 PUTG`

Operands: B
Pop/Push: 1/0

Store `$1` into program global `$B`. See §7.3.

---

### `0x5F 0x07 POPN`

Operands: -
Pop/Push: varies/0

Pop `$1` from the evaluation stack as a count, then discard that many
additional words from the evaluation stack.

---

### `0x5F 0x08` (reserved)

This extended opcode value is reserved. No behavior is defined for it by this
edition of the specification. The interpreter's behavior when executing a
reserved opcode is undefined.

---

### `0x5F 0x09 LONGJMPR`

Operands: -
Pop/Push: 2/1

Transfer control to the second procedure offset recorded in the activation
record associated with activation token `$2`, and push `$1` as a return value.
The state stored in the activation record is restored and the token is
invalidated. See §6.7.

---

### `0x5F 0x0A LONGJMP`

Operands: -
Pop/Push: 1/0

Transfer control to the first procedure offset recorded in the activation
record associated with activation token `$1`, without pushing a result. The
state stored in the activation record is restored and the token is invalidated.
See §6.7.

---

### `0x5F 0x0B SETJMP`

Operands: WW
Pop/Push: 0/1

Save the current execution state into an activation record, as described in
§6.7, and push the activation token. The two inline word operands are
procedure offsets within the current module. The first procedure offset
identifies the transfer target for a subsequent `LONGJMP`; the second
identifies the transfer target for a subsequent `LONGJMPR`.

---

### `0x5F 0x0C OPEN`

Operands: B
Pop/Push: 2/1

Open or attach a file channel using the string at handle `$2` as the filename.
The inline byte `$B` is the mode byte; see §8.1.2 for the mode byte encoding
and filename resolution rules.

> **Provisional:** The role of operand `$1` has not been determined. Its value
> may be ignored, or it may supply a secondary parameter to the open operation.

---

### `0x5F 0x0D CLOSE`

Operands: -
Pop/Push: 1/1

Close the channel `$1` and push a status result.

> **Provisional:** The meaning of the pushed status value has not been
> confirmed against MME. It is not known whether the status follows the same
> success/failure convention as `UNLINK` (0 on success, **FALSE** on failure)
> or uses a different encoding.

---

### `0x5F 0x0E READ`

Operands: -
Pop/Push: 3/1

Read `$1` words from channel `$2` into aggregate `$3`, starting after the
aggregate's initial length word. Push a status result.

> **Provisional:** The meaning of the pushed status value has not been
> confirmed against MME.

---

### `0x5F 0x0F WRITE`

Operands: -
Pop/Push: 3/1

Write `$1` bytes to channel `$2` from aggregate `$3`. For each byte written,
the low byte of one word in the aggregate is used and the high byte is
discarded; `$1` words are consumed from the aggregate to produce `$1` bytes.
Transfer starts at the beginning of the aggregate. Push a status result.

> **Provisional:** The meaning of the pushed status value has not been
> confirmed against MME.

---

### `0x5F 0x10 READREC`

Operands: -
Pop/Push: 4/1

Read `$1` I/O records from channel `$3` starting at record index `$2` into the
payload of vector `$4`. The record size is determined by system variable `0xCC`;
see §7.5.3 and §8.1.4. Push a status result.

> **Provisional:** The meaning of the pushed status value has not been
> confirmed against MME.

---

### `0x5F 0x11 WRITEREC`

Operands: -
Pop/Push: 4/1

Write `$1` I/O records from the payload of vector `$4` to channel `$3`
starting at record index `$2`. The record size is determined by system variable
`0xCC`; see §7.5.3 and §8.1.4. Push a status result.

> **Provisional:** The meaning of the pushed status value has not been
> confirmed against MME.

---

### `0x5F 0x12 DISP`

Operands: B
Pop/Push: 0/0

Perform the display operation selected by `$B`. For the complete list of
suboperations, see §8.2.4.

---

### `0x5F 0x13 XDISP`

Operands: B
Pop/Push: 1/1

Perform the area-relative display operation selected by `$B`, using `$1` as a
line count. For the complete list of suboperations and the system variables
that govern the scroll region boundaries, see §8.2.5. The line count `$1` is
pushed back onto the evaluation stack after the operation.

---

### `0x5F 0x14 FSIZE`

Operands: -
Pop/Push: 1/1

Push the size of the file accessed through open channel `$1`, expressed as the
number of 256-byte blocks (i.e., the file size divided by 256, rounded down).

---

### `0x5F 0x15 UNLINK`

Operands: -
Pop/Push: 1/1

Delete the file whose name is the string at handle `$1`, and close any open
channels to that file. Push `0` on success or **FALSE** on failure.

---

### `0x5F 0x16 POPRET`

Operands: -
Pop/Push: varies/0

Discard from the evaluation stack a number of words equal to the return-value
count of the most recently executed `RETURN`, `RFALSE`, `RZERO`, `RET`, or
`RETN` instruction.

---

### `0x5F 0x17 KBINPUT`

Operands: -
Pop/Push: 0/1

Poll the keyboard for input and push a key code if a key is ready, or
**FALSE** if no key is available.

If system variable `0xC0` is nonzero, the instruction returns **FALSE**
immediately without polling the keyboard; see §7.5.2.

When a special key produces an extended key code, it is exposed as two
consecutive key codes that must be read with two separate `KBINPUT` calls;
see §8.3.2 for the extended-key encoding.

---

### `0x5F 0x18 FADD`

Operands: -
Pop/Push: 3/1

Floating-point addition: write `real($3) + real($2)` into `real($1)`, then
push `$1`. See §4.7 for the floating-point representation.

---

### `0x5F 0x19 FSUB`

Operands: -
Pop/Push: 3/1

Floating-point subtraction: write `real($3) - real($2)` into `real($1)`, then
push `$1`. See §4.7.

---

### `0x5F 0x1A FMUL`

Operands: -
Pop/Push: 3/1

Floating-point multiplication: write `real($3) * real($2)` into `real($1)`,
then push `$1`. See §4.7.

---

### `0x5F 0x1B FDIV`

Operands: -
Pop/Push: 3/1

Floating-point division: write `real($3) / real($2)` into `real($1)`, then
push `$1`. See §4.7.

---

### `0x5F 0x1C TPOP`

Operands: -
Pop/Push: 1/0

Discard `$1` words from the tuple stack by advancing the internal tuple-stack
pointer. See §4.4 for the tuple allocator model.

---

### `0x5F 0x1D FLOG`

Operands: -
Pop/Push: 2/1

Floating-point natural logarithm: write `ln(real($2))` into `real($1)`, then
push `$1`. See §4.7.

---

### `0x5F 0x1E FEXP`

Operands: -
Pop/Push: 2/1

Floating-point exponential: write `e^real($2)` into `real($1)`, then push
`$1`. See §4.7.

---

### `0x5F 0x1F STRICMP`

Operands: -
Pop/Push: 5/1

Compare the string slice (`$4`, `$2`, `$1`) case-insensitively against the
string (`$5`, `0`, `$3`) without skipping spaces, and push the signed
comparison result; see §1.5.2.

The three-element slice notation is (handle, starting-byte-offset,
byte-count). The second operand uses a starting offset of `0` and the entire
string defined by `$3` bytes.

---

### `0x5F 0x20 STRICMP1`

Operands: -
Pop/Push: 5/1

Identical to `STRICMP`, except that the first string handle used is `$4 - 1`
rather than `$4`. Push the signed comparison result; see §1.5.2.

---

### `0x5F 0x21 WPRINTV`

Operands: -
Pop/Push: 4/1

Print a clipped substring of a source string into a window. `$4` is the window
descriptor handle `D`, `$3` is the source string handle `S`, `$2` is the
maximum character count `N`, and `$1` is the starting character offset `O`
within the string.

Push the logical column value from the descriptor, or **FALSE** if the window
has zero visible width. The full algorithm, including bound computation,
clipping, and the `SETWIN` call it performs implicitly, is defined in §8.4.4.

---

### `0x5F 0x22 SETWIN`

Operands: -
Pop/Push: 1/1

Activate the window described by the window descriptor at handle `$1`.
Initialise the cursor and display attributes from the descriptor vector, commit
the cursor position via system variables `0xCA` and `0xC9`, and mirror the text
attribute into system variable `0xD5`. Push the logical column value from the
descriptor. The full activation algorithm is defined in §8.4.3.

---

### `0x5F 0x23 KEYCMP`

Operands: -
Pop/Push: 2/1

Compare the structured sort keys at handles `$2` and `$1`. Push `1` if `$2`
is greater than or equal to `$1`, or `0` otherwise.

The comparison walks a series of field sequences in each key; see §8.4.4 (or
the full key structure definition below) for the field layout.

The key structure is:
- One tiebreaker byte `C`,
- One byte giving a total byte count `N`,
- Two tiebreaker bytes `B` and `A`, and
- `N` bytes containing a sequence of length-prefixed fields.

Each field is a length byte `L` followed by `L` bytes of data.

The comparison process is:
1. Compare corresponding bytes of the first field in each key. If any byte
   differs, the key with the lower byte is smaller; stop.
2. If all bytes match but one field is shorter, the shorter field is smaller;
   stop.
3. If all bytes and lengths match and both keys have more fields, advance to
   the next field and repeat from step 1.
4. If one key exhausts its fields first, that key is smaller; stop.
5. If bit 6 of `C` in the first key is clear, treat the keys as equal; stop.
6. Compute tiebreaker value `((A & 0x3F) << 8) + B` for each key. If they
   differ, the key with the lower value is smaller; stop.
7. Compute tiebreaker value `C & 0x3F` for each key. If they differ, the key
   with the lower value is smaller; stop.
8. Treat the first key as greater; stop.

> **Note:** `KEYCMP` uses a binary result (`1` for greater-or-equal, `0` for
> less-than), not the ternary signed comparison result used by `STRICMP`,
> `MEMCMP`, and `MEMCMPO`. The polarity is also opposite: `KEYCMP` returns
> `1` when the first operand is *not* smaller, while the signed comparison
> result returns `+1` when the first operand *is* smaller. Do not confuse
> these conventions.

---

### `0x5F 0x24 MEMCMP`

Operands: -
Pop/Push: 3/1

Compare `$1` bytes of aggregate `$2` against aggregate `$3` and push the
signed comparison result; see §1.5.2.

> **Note:** The sign convention used by this instruction is the opposite of the
> C library function `memcmp`: the result is `+1` when the first operand
> (`$2`) is *smaller*, and `−1` when it is *greater*.

---

### `0x5F 0x25 MEMCMPO`

Operands: -
Pop/Push: 4/1

Compare `$2` bytes of aggregate `$3`, starting at byte offset `$1`, against
aggregate `$4`, starting at byte offset `2` (skipping an initial length word),
and push the signed comparison result; see §1.5.2.

> **Note:** The sign convention is the opposite of C `memcmp`: the result is
> `+1` when the first operand (`$3`) is *smaller*, and `−1` when it is
> *greater*.

---

### `0x5F 0x26 ADVANCE`

Operands: -
Pop/Push: 2/1

Read the lengths of two consecutive length-prefixed fields from aggregate `$2`
starting at 1-based byte index `$1`, skip past both fields, and push the
resulting 1-based byte index immediately after the second field.

The aggregate is expected to contain, starting at index `$1`:
- A length byte `N`, followed by `N` bytes of data,
- A length byte `M`, followed by `M` bytes of data.

The returned index is `$1 + N + M + 2`.

---

### `0x5F 0x27 DECMG`

Operands: B
Pop/Push: 0/1

Decrement module global `$B` and push the new value. See §7.4.

---

### `0x5F 0x28 DECG`

Operands: B
Pop/Push: 0/1

Decrement program global `$B` and push the new value. See §7.3.

---

### `0x5F 0x29 POPI`

Operands: B
Pop/Push: varies/0

Discard `$B` words from the evaluation stack, where `$B` is the inline byte.

---

### `0x5F 0x2A INCG`

Operands: B
Pop/Push: 0/1

Increment program global `$B` and push the new value. See §7.3.

---

### `0x5F 0x2B INCMG`

Operands: B
Pop/Push: 0/1

Increment module global `$B` and push the new value. See §7.4.

---

### `0x5F 0x2C STOREMG`

Operands: B
Pop/Push: 0/0

Copy the top word of the evaluation stack into module global `$B`, leaving
the stack unchanged. See §7.4.

---

### `0x5F 0x2D STOREG`

Operands: B
Pop/Push: 0/0

Copy the top word of the evaluation stack into program global `$B`, leaving
the stack unchanged. See §7.3.

---

### `0x5F 0x2E RETN`

Operands: B
Pop/Push: varies/varies

Return from the current procedure, passing `$B` words from the evaluation stack
to the caller as a multi-word return value. The words are transferred in their
current stack order: the word that was topmost on the evaluation stack when
`RETN` executes is the topmost word of the return value as presented to the
caller. See §6.6.

---

### `0x5F 0x2F PRCHAR`

Operands: -
Pop/Push: 1/0

Print the character `$1` from code page 437 at the current cursor position,
then advance the cursor one position to the right. If the cursor is already at
the right edge of the screen, it remains there.

`$1` must be in the range `0–255`.

---

### `0x5F 0x30 UNPACK`

Operands: -
Pop/Push: 4/1

Unpack up to `$2` words of 5-bit symbols from aggregate `$4` into bytes in
aggregate `$3`, starting at destination byte offset `$1`, and push either
**FALSE** or the byte offset immediately following the last destination byte.

Each source word `W` is expanded into three destination bytes:
`(W >> 10) & 0x1F`, `(W >> 5) & 0x1F`, and `W & 0x1F`. Bit 15 of the source
word is a stop bit. If the stop bit is encountered, bit 7 is set on the final
destination byte and **FALSE** is pushed. If `$2` words are consumed without
encountering a stop bit, bit 7 is not set and `$1 + (3 * $2)` is pushed.

---

### `0x5F 0x31 PINM`

Operands: (not confirmed)
Pop/Push: (not confirmed)

> **Provisional:** The mnemonic `PINM` has been identified for this extended
> opcode, but the operands, stack effect, and semantics have not been confirmed
> against MME.

---

### `0x5F 0x32 UNPINM`

Operands: (not confirmed)
Pop/Push: (not confirmed)

> **Provisional:** The mnemonic `UNPINM` has been identified for this extended
> opcode, but the operands, stack effect, and semantics have not been confirmed
> against MME.

---

### `0x5F 0x33` (reserved)

This extended opcode value is reserved. No behavior is defined for it by this
edition of the specification. The interpreter's behavior when executing a
reserved opcode is undefined.

---

### `0x5F 0x34 FMTREAL`

Operands: -
Pop/Push: 4/1

Format the floating-point number in aggregate `$4` as a decimal string into
aggregate `$3`, using `$2` as the precision specifier and `$1` as format flags.
Push `$3`. See §4.7 for the floating-point representation.

If `$2` is not **FALSE**, the number is formatted as fixed-point with exactly
`$2` digits after the decimal point, with no leading zero before the decimal
point.

Otherwise, if bit 3 (`0x08`) of `$1` is set, the number is rounded to an
integer. If bit 3 is clear, the number may be formatted as an integer, a
floating-point number with up to 15 digits after the decimal, or a
scientific-notation form with up to 15 significant digits, depending on its
value.

---

### `0x5F 0x35 PRSREAL`

Operands: -
Pop/Push: 6/1

Parse a floating-point number from a source string into an 8-byte real buffer
and push the destination buffer handle or **FALSE**. See §4.7 for the
floating-point representation.

`$6` is the source text aggregate, `$5` is the destination 8-byte real buffer,
`$4` is the source byte count, `$3` is the source byte offset, `$2` is an
option word, and `$1` is the address of a two-byte result word.

`PRSREAL` accepts: plain integers, fixed-point decimals, `$`-prefixed amounts,
numbers with comma group separators, scientific notation using `E` or `e`, a
leading minus sign, and accounting-style negatives in parentheses. Examples:
`123`, `123.45`, `.99`, `$99.95`, `1,000`, `2.99e5`, `-16.5`, `(9.95)`.

Parsing stops at the end of a valid numeric prefix; trailing non-numeric
characters do not cause failure. Callers may detect trailing junk by comparing
the consumed count written into the result word against the supplied source
length.

On success: write the parsed value into `real($5)`, write the consumed
character count into result byte `$1[0]`, write a status code into result byte
`$1[1]`, and push `$5`.

On failure: write the consumed count into result byte `$1[0]`, write an
error/status code into result byte `$1[1]`, and push **FALSE**.

Status codes:

- `0x01`: successful parse.
- `0x02`: an exponent marker (`E` or `e`) was seen but no exponent digits
  followed it.

Additional status codes may exist. Any code other than `0x01` indicates
failure.

---

### `0x5F 0x36 LOOKUP`

Operands: -
Pop/Push: 1/1

Resolve a packed descriptor key through a two-level descriptor table. The
algorithm is defined in §8.5.2.

---

### `0x5F 0x37 EXTRACT`

Operands: -
Pop/Push: 8/1

Extract data from a structured aggregate using a multi-mode descriptor walker.
The algorithm is defined in §8.5.3.

---

### `0x5F 0x38 RENAME`

Operands: -
Pop/Push: 2/1

Rename the file whose name is the string at handle `$2` to the name given by
the string at handle `$1`. Push `0` on success or **FALSE** on failure.

---

### `0x60–0x9F VREAD_loc_vec`

Operands: -
Pop/Push: 0/1

Packed vector-word read. The opcode byte itself encodes both the local variable
index and the visible-index position to read:

- Bits 3–0 select the local variable (0–15) whose value is the aggregate
  handle.
- Bits 5–4 select one of the first four visible indices (1–4) within the
  aggregate.

See §5.4.1 for the full bit-layout encoding of this packed opcode family and
§4.5 for the definition of **visible index**.

---

### `0xA0–0xBF PUSH_Ln`

Operands: -
Pop/Push: 0/1

Packed local push. The opcode byte encodes a local variable index `n` in bits
4–0 (range 0–31); push the value of local variable `n`. See §5.4.2.

---

### `0xC0–0xDF PUT_Ln`

Operands: -
Pop/Push: 1/0

Packed local store. The opcode byte encodes a local variable index `n` in bits
4–0 (range 0–31); pop the top word of the evaluation stack and store it into
local variable `n`. See §5.4.3.

---

### `0xE0–0xFF STORE_Ln`

Operands: -
Pop/Push: 0/0

Packed local copy. The opcode byte encodes a local variable index `n` in bits
4–0 (range 0–31); copy the top word of the evaluation stack into local variable
`n` without consuming it. See §5.4.4.

---
