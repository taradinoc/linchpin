# Cornerstone Virtual Machine Specification

## Version 0.1

*Tara McGrew*\
*March&ndash;April 2026*

## 1. Introduction

### 1.1 Purpose

This document defines the **Cornerstone virtual machine**, historically identified as the **Mu Machine** and implemented as **MME** (Mu Machine Emulator). It specifies the contract between the interpreter and the bytecode program it interprets.

This specification is intended to be useful for:

1. parsing `.MME` and `.OBJ` image files;
2. disassembling valid Cornerstone bytecode images; and
3. implementing an interpreter capable of accurately executing the Cornerstone database package and other existing program images.

### 1.2 Scope

The Cornerstone virtual machine is a 16-bit interpreted bytecode architecture. A complete executable image consists of both:

1. a `.MME` file, which contains the image header, several metadata tables describing modules and their contents, and the initial RAM image; and
2. a `.OBJ` file, which contains the modules and read-only data.

From the standpoint of bytecode, the machine provides a stack-based execution architecture with procedures, locals, globals, vectors, tuples, and non-local control transfer, as well as several services including memory allocation, display control, keyboard polling, stream- and record-based I/O, and complex data searching and extraction.

All of the above is within the scope of this specification.

Application-level data formats are outside the scope of this specification, except for those that are manipulated by the interpreter.

### 1.3 Notation

In this specification:

* Hexadecimal numbers are written in C syntax, such as `0xAB`
* **Bold** is used for the primary definition of a term or structure
* _Italic_ is used for emphasis
* Offsets are specified in bytes, unless otherwise specified
* Values are implied to be words, unless otherwise specified

This specification uses ordinal numbers rather than numeric indexes when referring to elements which are potentially subject to multiple indexing systems. For example, the initial word of a vector, which may have index `0` or `1` depending on how it is accessed, is described here as the "first word".

### 1.4 Terminology

For the purposes of this specification:

* **FALSE** is the value `0x8001`, *not zero*.
* a **word** is a 16-bit little-endian value;
* a **byte** is an 8-bit value;
* **bits** within a byte are numbered 0 to 7, with 0 being the least significant bit;
* a **module number** is 1-based;
* an **index** is 1-based unless otherwise specified;
* an **offset** is zero-based;
* a **logical address** is a 16-bit VM-visible word address interpreted according to the split-RAM mapping defined in section 6.2;
* a **handle** is a logical address pointing to the beginning of a vector or tuple;
* a **vector** is a consecutive group of words in RAM;
* a **length-prefixed vector** is a vector whose first element is the count of the words that follow;
* a **string** is a mixed-element structure in memory whose first *word* is the count of the *bytes* that follow;
* a **procedure** or **function** is a block of code starting at a given offset within a module in the `.OBJ` file, consisting of a header and instructions;
* a **procedure selector** is a 16-bit far procedure reference resolved through the procedure tables defined in the `.MME` image, identifying a procedure exported from a module;
* the **VM** or **virtual machine** is the abstract computer described in this specification;
* an **interpreter** is any implementation of the virtual machine described in this specification;
* the **bytecode** or **program** is the software running within the VM (such as the Cornerstone database package), described in the `.MME` and `.OBJ` files;
* the **RAM** is the word-addressed memory made available to the bytecode, including but not limited to the memory initialized from the initial RAM image;
* a **VM-visible** value or behavior is one that can be detected by a conforming bytecode program running within the VM;

### 1.4 Normative Language

The key words **must**, **must not**, **shall**, **shall not**, **should**, **should not**, **recommended**, **may**, and **optional** in this document are to be interpreted as described in RFC 2119.

Unless explicitly marked otherwise, statements in this document are normative.

Behavior that is not defined by this specification is implementation-dependent. A **conforming** program is one that behaves as required by this specification, and does not rely on any undefined behavior from the interpreter.

### 1.5 Reference Implementations

In addition to the canonical implementation, MME, there exist two reference implementations, Linchpin (C#) and LinchpinST (C), available as of April 2026 at <https://github.com/taradinoc/linchpin>, which should be consulted if required information is found to be missing from this specification.

### 1.6 Completeness/Correctness

This specification is the result of a third-party reverse-engineering effort undertaken more than 40 years after Cornerstone's release. At the time of this writing, that effort is still ongoing, and existing Cornerstone program images may expect interpreter behavior that remains undiscovered or undocumented. Future versions of this specification may define behavior that is undefined in this version.

Except where noted, if the behavior of MME contradicts this specification, its behavior should be considered correct, and the specification incorrect.

## 2. Architectural Overview

### 2.1 Execution Model

The Cornerstone virtual machine is a stack machine. Instructions consume operands from an evaluation stack of words and produce results either on that stack or into one of the VM storage spaces.

The principal storage spaces are:

1. locals in the current procedure frame;
2. program globals;
3. module globals;
4. managed vectors and tuples; and
5. interpreter-owned system slots exposed through the high out-of-range module-global band.

### 2.2 Module Organization

Code is divided into **modules**. The modules are stored in the `.OBJ` file; the last module may optionally be followed by read-only data. Module boundaries are defined by the module table in the `.MME` file.

In order for one module to call procedures in another, a far call instruction must be used with the called procedure's selector, and the called procedure's code offset must be listed in the module's **export table** in the `.MME` file. The procedures in the export table may be in any order, regardless of the order of the procedures in the `.OBJ` file.

Procedures within the same module may call each other by using a near call instruction, which takes the called procedure's code offset within the module.

The number of modules is limited to 24. Each module may have any number of private (non-exported) procedures, but no more than 255 exported procedures, and no more than 64 KB of code.

### 2.3 Code Blocks

Execution is organized into 256-byte code blocks.

Every code block must terminate with a `NEXTB` instruction. Each block shall be padded as necessary so that the following block begins on a 256-byte boundary.

A procedure may span any number of 256-byte boundaries, provided that each individual instruction and each procedure header lies entirely within a single block. Individual instructions and procedure headers shall not cross a 256-byte boundary.

### 2.4 Fetch and Dispatch

The interpreter shall fetch and dispatch one primary opcode byte at a time from the active instruction stream.

Extended opcodes use the primary byte `0x5F`, followed by a one-byte secondary opcode. The defined extended-opcode range is `0x00..0x38`.

## 3. Program Image Format

### 3.1 The `.MME` Container

The `.MME` file is the primary metadata container for a Cornerstone image. It contains the header, module table, global variable tables, module export tables, and initial RAM image.

#### 3.1.1 Header

The `.MME` file has a 48-word (96-byte) **image header**, most of which is reserved. The following fields are defined, all of which are little-endian words:

| Byte Offset | Meaning |
|:-----------:|---------|
| `0x0E`        | The total size in bytes of all modules in the `.OBJ` file, divided by 512 and rounded down |
| `0x10`        | The procedure selector of the entry point |
| `0x16`        | The size in words of the initial RAM image |
| `0x1A`        | The offset in the `.MME` file of the module table, divided by 256 |
| `0x1E`        | The length of the module table, in words |

The initial portion of the header at byte offsets `0x00..0x0C` is considered the **image preamble**. Its semantics are not fully defined by this specification. An image emitter targeting the Cornerstone VM should emit the following preamble bytes: `0x65, 0x00, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x16, 0x00, 0xAB, 0x00`. The remaining undefined bytes in the header should be filled with zeros.

#### 3.1.2 Module Table

The header is followed by the **module table**, which defines the number and location of the code modules:

| Byte Offset | Meaning |
|:-----------:|---------|
| `0x60`        | The number of modules |
| `0x62`        | The offset of module 1 in the `.OBJ` file, divided by 256 |
| ...         | ... |
| `0x60 + 2N`   | The offset of module N in the `.OBJ` file, divided by 256 |

The number of modules must not exceed 24.

#### 3.1.3 Global Variable Tables

The module table is followed by the **global variable size table**, which defines the number of **global variables** and **module global variables** available to the bytecode:

| Byte Offset | Meaning |
|:-----------:|---------|
| `+0` | The number of global variables |
| `+2` | The number of module-global variables for module 1 |
| ...  | ... |
| `+2N` | The number of module-global variables for module N |

The number of global variables must not exceed 255.

The global variable size table is followed by the **initial global variable table**, which contains the initial values of the global variables, in the same order as the table above:

| Byte Offset | Meaning |
|:-----------:|---------|
| `+0` | The initial values of the global variables, in order |
|      | The initial values of the module-global variables for module 1, in order |
|      | ...  |
|      | The initial values of the module-global variables for module N, in order |

#### 3.1.4 Module Export Tables

The global variable tables are followed, at the offset indicated by the image header word at `0x1A`, by the **module export tables**. This table is repeated for each module in the program, in order:

| Byte Offset | Meaning |
|:-----------:|---------|
| `+0` | The number of procedures exported by the module |
| `2`  | The offset within the module of exported procedure 1 |
| ...  | ... |
| `2N` | The offset within the module of exported procedure N |

The number of procedures exported by each module must not exceed 256.

The last module export table is always followed by at least one padding byte.

### 3.2 Initial RAM Image

The **initial RAM image** begins at the next 256-byte boundary after the padding byte following the last module export table. There must be at least one padding byte in between; if the module export table ends at a 256-byte boundary, it must be followed by 256 padding bytes.

The size of the initial RAM image in words is given by the image header word at `0x16`. The initial RAM image must not exceed 32,768 words (64 KB).

The size of the initial RAM image should be kept to a minimum because of the split RAM model described in section 6.2 of this specification. If the size of the initial RAM exceeds the amount of memory allocated to the low RAM segment _at runtime_, the excess will be placed in the high segment, invalidating the compiled addresses of any structures located there.

### 3.3 `.OBJ` Module Layout

The `.OBJ` file contains all of the modules of the program. Each module must start on a 256-byte boundary. Their offsets are given in the module table in the `.MME` file.

The modules may be followed by read-only data, which will be made available to the program at runtime through the channel indicated by the low bits of system variable `0xDA`.

## 4. Procedures, Frames, and Control Transfer

### 4.1 Procedure Header Format

Every procedure begins with a **procedure header**. The first header byte is interpreted as follows:

* bits `0-6`: local variable count (0-63);
* bit `7`: flag indicating whether initial values are present.

If bit 7 is set, the first header byte is followed by one or more initializer records. Each initializer record starts with an introduction byte:

* bits `0-5`: local variable index (0-63);
* bit `6`: flag indicating the size of the initial value;
* bit `7`: flag indicating the final initializer record.

The introduction is followed by one or two bytes giving the initial value of the local variable. If bit 6 of the introduction is set, the initial value is a byte that must be sign-extended; if bit 6 is clear, the initial value is a word.

Each initializer record names one local variable and supplies one initializer value. The list ends with the record that has bit 7 set.

The combined size of all the initializer records must not exceed 255 bytes.

### 4.2 Procedure Boundaries

A procedure may start at any offset within a module, provided that the procedure header does not cross a 256-byte boundary. Padding bytes may be present between procedures.

The length of a procedure is not specified, but a procedure may be presumed to end before the next higher offset listed in the module's export table.

### 4.3 Call Forms

The instruction set defines two call families:

1. near calls: `CALL0` through `CALL3`, and `CALL`;
2. far calls: `CALLF0` through `CALLF3`, and `CALLF`.

A near call remains within the current module and uses a module-relative procedure offset.

A far call uses a **procedure selector** word with the following layout:

* high byte: 1-based module number;
* low byte: zero-based procedure-table index within that module.

Fixed-arity and computed forms differ only in how argument count and selector are supplied:

* `CALL0..CALL3` and `CALLF0..CALLF3` encode the argument count in the opcode form itself and the offset/selector in an inline word operand;
* `CALL` and `CALLF` take the argument count from an inline byte operand and the offset/selector from the stack.

### 4.4 Argument Mapping and Local Initialization

Before executing `CALL*` or `CALLF*`, the caller may place one or more arguments on the stack.

The callee's local variables shall be initialized as follows:

1. arguments are popped from the stack and stored in local variables in reverse order, such that the first argument pushed becomes the first local variable;
2. all remaining local variables are initialized to the values specified by the procedure header's initializer records, or FALSE (`0x8001`) if not specified.

Procedure-header initializers shall not overwrite arguments passed by the caller.

### 4.5 Call Frames

An ordinary call creates a new local frame and preserves sufficient continuation state to resume the caller in the correct module and code position. The frame persists until the procedure is exited by a return instruction or non-local control transfer instruction.

The implementation of call frames is defined by the interpreter, and not VM-visible.

The exact in-memory frame layout used by a particular interpreter is implementation-defined.

### 4.6 Return Forms

An ordinary return resumes execution at the caller’s continuation point, at the instruction following the call instruction, and exposes the returned value or values directly on the caller’s stack.

The return-family opcodes define the following result forms:

* `RETURN`: return one word from the stack;
* `RFALSE`: return `0x8001`;
* `RZERO`: return `0`;
* `RET`: return no value;
* `RETN`: return any number of words from the stack, with the count given as an inline operand;
* `POPRET`: discard the stack words returned by the last return instruction.

When multiple words are returned by `RETN`, their order is preserved, such that the last word pushed by the callee becomes the first word popped by the caller.

The number of words discarded by `POPRET` is set when a return instruction is executed, and is not VM-visible outside of its effect on `POPRET`. The effect of executing `POPRET` more than once between return instructions is undefined.

### 4.7 Process Exit

The `HALT` instruction is the only valid method for terminating the program. The program must not return from the entry procedure.

`HALT 1` denotes normal termination. After terminating the program, the interpreter may clear the screen.

Any other `HALT` value denotes abnormal termination. After terminating the program, the interpreter should report the value to the user as an error code.

### 4.8 Non-Local Control Transfer

The non-local control-transfer family consists of `SETJMP`, `LONGJMP`, and `LONGJMPR`.

`SETJMP` saves a record containing:

1. the depth of the tuple stack;
2. the depth of the evaluation stack;
3. the two code addresses in the current module, provided as inline operands;
4. the current module context; and
5. the current local variable frame (not the contents of local variables).

The record is not VM-visible, and is only valid until the function containing `SETJMP` returns. The instruction pushes an activation token.

`LONGJMP` shall:

1. consume the activation token;
2. restore the tuple stack depth;
3. restore the evaluation stack depth;
4. restore the module context;
5. restore the local variable frame;
6. set the program counter to the _first_ saved code address; and
7. resume execution without preserving a distinct explicit return value.

`LONGJMPR` shall:

1. consume the activation token;
2. preserve one word from the stack as a return value;
3. restore the tuple stack depth;
4. restore the evaluation stack depth;
5. restore the module context;
6. restore the local variable frame;
7. set the program counter to the _second_ saved code address; and
8. push the preserved return value before resuming execution.

## 5. Instruction Stream and Encoding

### 5.1 Opcode Organization

The bytecode space is partitioned as follows:

* `0x00-0x5E`: base opcodes;
* `0x5F xx`: extended opcode prefix and secondary opcode byte;
* `0x60-0x9F`: packed `VREAD__` family;
* `0xA0-0xBF`: packed `PUSH_Ln` family;
* `0xC0-0xDF`: packed `PUT_Ln` family;
* `0xE0-0xFF`: packed `STORE_Ln` family.

This partition is sufficient to determine instruction form, mnemonic family, length, and namespace semantics.

### 5.2 Encoding Rules

The following scalar encoding rules are fixed:

1. inline words are little-endian;
2. base opcodes are followed by zero or more inline operands, with the number and size depending on the opcode;
3. extended opcodes always begin with `0x5F` followed by a one-byte secondary opcode;
4. packed families consume no inline operand bytes.

The inline operands of jump instructions are variable size. If the first byte following the opcode is nonzero, it is interpreted as a signed code offset relative to the start of the next instruction. If the first byte following the opcode is `0x00`, that byte is followed by a word, which is interpreted as an absolute code address in the current module.

### 5.3 Base-Opcode Operand Classes

Base opcodes use the following operand classes:

* no inline operand: all base opcodes not otherwise listed below;
* one unsigned byte: `INCL`, `DECL`, `VLOADW_`, `VLOADB_`, `VPUTW_`, `VPUTB_`, `LOADG`, `LOADMG`, `PUSHB`, `PUTMG`, `INCLV`, `PUTL`, `PUSHL`, `STOREL`;
* one little-endian word: `PUSHW`, `HALT`, `BITSVL`, `BITSV`, `BBSETVL`, `BBSETV`, `BSETVL`, `BSETV`;
* mixed jump target: all jump opcodes `0x30-0x3E`;
* fixed near-call code address: `CALL0` through `CALL3`;
* one unsigned-byte argument count: `CALL` and `CALLF`;
* fixed far-call selector word: `CALLF0` through `CALLF3`;
* implicit block padding: `NEXTB`, which consumes the remaining bytes in the current 256-byte block.

### 5.4 Extended-Opcode Operand Classes

After the `0x5F` prefix and secondary opcode byte, extended opcodes use the following operand classes:

* no inline operand: all extended opcodes not otherwise listed below;
* one unsigned byte: `PUTG`, `OPEN`, `DISP`, `XDISP`, `DECMG`, `DECG`, `POPI`, `INCG`, `INCMG`, `STOREMG`, `STOREG`, `RETN`;
* two code addresses: `SETJMP`.

### 5.5 Packed One-Byte Families

The packed opcode families are defined by opcode bit fields.

#### 5.5.1 `0x60-0x9F`: `VREAD_loc_vec`

* bits `0-3` select the local slot containing the aggregate handle;
* bits `4-5` select one of the first four visible word slots to read.

#### 5.5.2 `0xA0-0xBF`: `PUSH_Ln`

* bits `0-4` select local slots `0..31`.

#### 5.5.3 `0xC0-0xDF`: `PUT_Ln`

* bits `0-4` select local slots `0..31`.

#### 5.5.4 `0xE0-0xFF`: `STORE_Ln`

* bits `0-4` select local slots `0..31`.

### 5.6 Floating-Point Operations

Several opcodes work with floating-point numbers, which are encoded as *big-endian* 64-bit IEEE754 doubles. Interpreters running on little-endian systems will likely need to reverse the bytes of floating-point values after reading them from VM memory and before writing them to VM memory.

The numbers themselves are stored in RAM, and the arguments passed to the opcodes are their handles. The bytecode program is responsible for allocating all memory used to hold the source and destination numbers.

## 6. Runtime State, VM Memory, and Aggregates

### 6.1 Execution State

The following elements define the execution state, and should be maintained by the interpreter:

1. current module context;
2. current program counter;
3. current procedure-frame and operand-stack state;
4. tuple-stack pointer state; and
5. allocator state for the low and high RAM halves.

The exact method/location of storage is implementation-defined. The allocator state may be stored in VM-visible RAM, but is not required to be.

### 6.2 Split Logical RAM Model

The VM exposes a 16-bit logical word-address space covering 128 KiB, distributed across two 64-KiB segments.

Logical addresses are interpreted as follows:

1. addresses `0x0000..0x7FFF` select the low RAM segment;
2. addresses `0x8000..0xFFFF` select the high RAM segment;
3. the lower fifteen bits select the word slot within the chosen segment;
4. word access uses byte offset `2 * (logical_address & 0x7FFF)` within the chosen segment;
5. byte access uses the same segment-selection rule and then applies a byte displacement within that word-addressed space.

Although the address space covers 128 KB, the amount of memory available to the VM at runtime may be less than 128 KB. In that case, the available memory is still split into two equal segments, one starting at logical address `0x0000` and the other starting at logical address `0x8000`, leaving a gap of addresses that are not backed by memory. The result of accessing addresses in that gap is undefined.

### 6.3 Vector and Tuple Allocation

At addresses past the end of the initial RAM image, memory is managed by one of two allocators: the vector allocator and the tuple allocator.

At program startup, a contiguous block of memory at the end of the high memory segment is designated for the tuple stack and managed by the tuple allocator.

The tuple allocator allocates memory starting from the high end of the stack and continuing downward, and may be implemented simply by tracking a pointer to the "top" of the stack.

The rest of RAM, excluding the tuple stack and the initial RAM image, is a heap managed by the vector allocator. The vector allocation opcodes may allocate memory from either the low or high segment, provided enough free memory is available.

Memory is always allocated in whole words.

Memory allocated for vectors is only reclaimed when explicitly returned to the allocator with the `VFREE` opcode. The program must keep track of the size of the allocated vector, and pass the same size to `VFREE` when returning the memory. The interpreter may validate the size.

Memory allocated for tuples may be returned to the allocator explicitly by the `TPOP` opcode, or implicitly by the `LONGJMP` and `LONGJMPR` opcodes.

The interpreter may keep its memory allocation structures in VM-visible memory. Before the program starts, the interpreter may write memory allocation structures anywhere in the high segment, and after the initial RAM image in the low segment. During execution, the interpreter may write into any unallocated or freed memory outside of the initial RAM image. The result of accessing that memory is undefined.

### 6.5 Aggregate Handles and Visible Layout

Several opcodes are designed to work with either statically allocated arrays in the initial RAM image, dynamically allocated vectors, or dynamically allocated tuples. Collectively, these structures are known as **aggregates**. The word address of an aggregate may be referred to as a **handle**.

Aggregates are generally accessed through 1-based indexes. However, the opcodes that take indexes as inline operands (`VLOADW_`, `VPUTW_`, `VLOADB_`, `VPUTB_`) use 0-based indexes.

Opcodes vary in whether they expect an aggregate to start with a "length word"; the name reflects the traditional usage, but the length word may generally be used for purposes other than storing a length. Opcodes that expect a length word start their index numbering with the word after the length word: index 0 is the length word, and index 1 is the following word.

The primary opcodes for accessing individual words (`VLOADW`, `VLOADW_`, `VPUTW`, `VPUTW_`) do not expect a length word, and they start their index numbering with the first word of the aggregate.

The opcodes for accessing individual bytes (`VLOADB`, `VLOADB_`, `VPUTB`, `VPUTB_`) always expect a length word, and they start their index numbering with the byte after the length word, which is the third byte of the aggregate.

### 6.6 RAM Strings

Strings stored in VM RAM and used by the interpreter use a Pascal-style layout:

1. first word: length of the string in bytes;
2. bytes following the length word: string content.

This matches the format expected by `VLOADB` and similar opcodes.

## 7. Storage Spaces and System Slots

### 7.1 Storage Classes

Other than the evaluation stack, the data directly visible to the program consists of:

1. local variables;
2. global variables;
3. module-global variables;
4. system variables; and
5. RAM.

Each of these classes, except system variables, has its own numbering space. Local variable 1, global variable 1, module-global variable 1, and RAM address 1 all coexist separately.

### 7.2 Program Globals and Module Globals

Dedicated opcode families access **global variables** and **module-global variables**.

Global variables have the same values regardless of which procedure or module is accessing them. They are accessed with the `LOADG`, `PUTG`, `INCG`, and `DECG` opcodes.

The number of global variables is determined by metadata in the `.MME` file, as are their initial values. The maximum number of global variables is 256.

Module-global variables have separate values in each module of the program. They are accessed with the `LOADMG` and `PUTMG` opcodes.

The number of module globals, which can differ between modules, is determined by metadata in the `.MME` file, as are their initial values. The maximum number of module globals is 192 per module.

Module globals share a numbering space and opcode family with system variables. Only variables `0x00..0xBF` are module globals.

### 7.3 System Variables

System variables occupy the top 64 slots of the module global numbering space, `0xC0..0xFF`, and are accessed with the same opcodes, `LOADMG` and `PUTMG`. They have the same values regardless of which procedure or module is accessing them.

Some system variables are read-only. The effect of writing to a read-only system variable is undefined.

Some system variables trigger an effect when a value is written to them.

Not all slots in the system variable range are in use. The following system variables are defined:

* `0xC0`: set to nonzero to disable `KBINPUT`;
* `0xC1`: set to FALSE to disable screen output; (???)
* `0xC4`, `0xC5`: bottom and top row limits used by bounded scrolling;
* `0xC7`: screen width, columns - 1;
* `0xC8`: screen height, rows - 1;
* `0xC9`, `0xCA`: cursor column and row backing words; writing `0xC9` commits the cursor move;
* `0xCC`: record size used by `READREC` and `WRITEREC`.
* `0xCD`, `0xCE`, `0xCF`: current month, day, and year;
* `0xD0`, `0xD1`, `0xD2`: current hour, minute, and second;
* `0xD3`: descriptor set by opcode `SETWIN`;
* `0xD4`: Ctrl+Break flag;
* `0xD5`: current text printing attributes;
* `0xD6`: read-only caps/num lock status word;
* `0xD7`: selected file extension, see below;
* `0xDA`: `.OBJ` access word, see below;
* `0xDB`: bit 15 set = beep is enabled, bit 0 set = color is enabled.

The following system variables may be used for implementation-dependent runtime statistics. An interpreter may leave them unimplemented, but should not use them for a different purpose:

* `0xE7`: statistics: `#outc`
* `0xE8`: statistics: `#outs`
* `0xE9`: statistics: `#curpos`
* `0xEA`: statistics: `#disp`
* `0xEB`: statistics: `#xdisp`
* `0xEC`: statistics: `#gets`
* `0xED`: statistics: `#sets`
* `0xEE`: statistics: `#hsets`
* `0xEF`: statistics: `#vsets`

### 7.3.1 System Variable 0xD7 (Selected File Extension)

Slot `0xD7` packs a 3-character string into a word as follows:

```
enc(str[0]) << 11 | enc(str[1]) * 45 | enc(str[2])
enc(char) is:  0-25 for A-Z
              26-35 for 0-9
              36-44 for $ & # @ ! % - _ /  respectively
```

This string represents a file extension chosen by the user. The default extension is `DBF`.

### 7.3.2 System Variable 0xDA (`.OBJ` Access Word)

Slot `0xDA` packs the `.OBJ` file's channel number and the approximate start of the read-only data into a word as follows:

`(channel of OBJ file) | (header word at 0x0E << 5)`

The header word at `0x0E`, in turn, is the offset of the start of read-only data divided by 512 (shifted right 9 bits).

## 8. Host Services

### 8.1 Channel and Record I/O

The VM exposes files through a channel abstraction. Files can be opened in read-only or read/write mode.

There are 25 potential channels, numbered `0..24`. One channel is opened by the interpreter before the program starts; its channel number is given by the low bits of system variable `0xDA`. Handle 24 is reserved for the printer.

The channel operations are defined as follows:

* `OPEN`: opens a file by name, in a given mode, optionally clobbering the file if it exists or creating it if it doesn't, and returns a channel value on success or FALSE on failure;
* `CLOSE`: closes a channel;
* `READ`: transfers a given number of words from a channel into a destination vector;
* `WRITE`: writes a given number of words from a source vector into a channel;
* `READREC count, record, channel, vector`: transfers record-oriented data into the destination vector byte payload beginning two bytes past the aggregate base;
* `WRITEREC count, record, channel, vector`: transfers record-oriented data from the source vector byte payload;
* `FSIZE`: returns file size in block-like units;
* `UNLINK`: deletes a named file;
* `RENAME`: renames one named file to another.

For record transfer:

1. system slot `0xCC` supplies the record size for `READREC` and `WRITEREC`;
2. fixed block transfers elsewhere in the subsystem use `0x200`-byte blocks.

#### 8.1.1 `OPEN` Mode Byte

The following mode bits are used by the `OPEN` opcode:

Low bits `0-1` select the access family:

* `mode & 0x03 == 0x00`: open for reading;
* `mode & 0x03 == 0x01`: delete or create the target first, then open for writing;
* `mode & 0x03 == 0x02`: open for read/write;
* `mode & 0x03 == 0x03`: delete or create the target first, then open for read/write.

Additional bits affect the filename resolution process as follows:

* bit 2 (`0x04`): enable search type 1;
* bit 4 (`0x10`): enable search type 2;

If neither bit is set, search type 0 is performed. If both bits are set, search type 1 takes precedence.

#### 8.1.2 `OPEN` Filename Resolution

The precise meanings of search types 0, 1, and 2 are not defined by this specification, except that type 1 should be the most exhaustive and type 0 the least exhaustive.

The interpreter may recognize `PRN` as a special filename representing a printer, and `LPT` followed by a digit as aliases for `PRN`.

### 8.2 Display-Control Services

Display control is primarily provided by `DISP` and `XDISP`.

`DISP` suboperations are:

* `DISP 0`: move cursor right one column;
* `DISP 1`: move cursor left one column;
* `DISP 2`: move cursor down one row;
* `DISP 3`: move cursor up one row;
* `DISP 4`: erase from the current column to the right edge of the active 80-column line, then restore the original cursor position;
* `DISP 5`: clear from the current row to the bottom of the active display area.

`XDISP` suboperations are:

* `XDISP 0`: scroll the area from the cursor row to the bottom row (system variable `0xC4`) downward by `$1` lines, or clear the area when `$1` is zero;
* `XDISP 1`: scroll upward over the same range, or clear the area when the count is zero;
* `XDISP 4`: like `XDISP 0` but uses the row in system variable `0xC5` instead of the cursor row;
* `XDISP 5`: like `XDISP 0` but uses the row in system variable `0xC5` instead of the cursor row;
* `XDISP 6`: draw a horizontal line `$1` characters wide starting at the cursor.

### 8.3 Keyboard Polling

`KBINPUT` is a nonblocking keyboard poll.

Its VM-visible behavior is:

1. if no key is ready, it pushes **FALSE**;
2. if input is ready, it returns a key code.

A blocking read may be constructed by looping with `KBINPUT` and `JUMPF`.

System slot `0xC0` acts as an input-enable gate. When nonzero, `KBINPUT` shall return **FALSE** immediately.

Extended keys are returned as `0x1B` followed by a mapped second byte on the next `KBINPUT` call. The following key sequences are defined:

* Escape: `0x1B 0x1B`;
* Shift+Tab: `0x1B 0x0F`;
* Up: `0x1B 0x48`;
* Down: `0x1B 0x50`;
* Left: `0x1B 0x4B`;
* Right: `0x1B 0x4D`;
* Home: `0x1B 0x47`;
* End: `0x1B 0x4F`;
* Page Up: `0x1B 0x49`;
* Page Down: `0x1B 0x51`;
* Insert: `0x1B 0x52`;
* Delete: `0x1B 0x53`;
* F1 through F10: `0x1B 0x3B` through `0x1B 0x44`;
* F11: `0x1B 0x85`;
* F12: `0x1B 0x86`.

### 8.4 Caps/Num Lock State

System slot `0xD6` exposes the state of Caps Lock and Num Lock. The visible bit assignments are:

* bit `0`: Num Lock;
* bit `1`: Caps Lock.

### 8.5 Text Output and Cursor Update

The interpreter does not process CR or LF characters as cursor moves. The bytecode program must move the cursor to the next line as needed.

The interpreter tracks a "virtual" cursor position, but does not move the "physical" cursor until a character is printed or `KBINPUT` is executed. (???)

System variable `0xCA` contains the cursor row, and `0xC9` contains the cursor column. Writing to `0xC9` commits the current value of `0xCA` and the new value of `0xC9` to the virtual cursor position.

### 8.6 Display Backends and Attributes

System slot `0xD5` contains the active text attributes. This slot can be written manually, and is also updated automatically from `D[2]` when executing `SETWIN`.

Only bits `0`, `3`, and `5` of `0xD5` contribute to the visible attribute:

* bit `0`: enable reverse video;
* bit `3`: enable bright/bold rendering;
* bit `5`: enable blink rendering.

### 8.7 Color Mode

System slot `0xDB` controls whether the interpreter uses a color or monochrome palette. Bit `0` is set for color or cleared for monochrome.

At startup, the monochrome palette is active.

### 8.8 Descriptor-Oriented Helpers

Extended helpers `0x5F 0x21` and `0x5F 0x22` belong to the descriptor and display subsystem.

This edition assigns the following names:

* `0x5F 0x21`: `WPRINTV`;
* `0x5F 0x22`: `SETWIN`.

Their semantics are defined in section 8.13.

### 8.9 Window Descriptors, `WPRINTV`, and `SETWIN`

`SETWIN` activates a display window described by descriptor vector `D`.

Stack input:

* one word: descriptor handle `D`.

Result:

* on success, return `D[0]`;
* on failure, return **FALSE** and clear the active-descriptor latch.

`WPRINTV` is a clipped text-painting operation.

Stack inputs are:

* `$4 = D`: display descriptor vector;
* `$3 = S`: source byte vector;
* `$2 = N`: requested character count;
* `$1 = O`: source byte offset.

The descriptor vector `D` has the following structure:

* `D[0]`: logical column;
* `D[1]`: logical row;
* `D[2]`: attribute flags word;
* `D[3]`: handle of geometry vector `G`.

The geometry vector `G = D[3]` has the following structure:

* `G[0]`: logical column origin;
* `G[1]`: logical row origin;
* `G[2]`: physical column base;
* `G[3]`: physical row base;
* `G[4]`: column span, exclusive upper bound for `D[0] - G[0]`;
* `G[5]`: row span, exclusive upper bound for `D[1] - G[1]`.

`SETWIN` shall validate and map coordinates as follows:

* `col_delta = D[0] - G[0]`;
* `row_delta = D[1] - G[1]`;

The descriptor is valid if and only if:

* `0 <= col_delta < G[4]`; and
* `0 <= row_delta < G[5]`.

On success:

* `physical_col = G[2] + col_delta`;
* `physical_row = G[3] + row_delta`.

`SETWIN` shall then:

1. record the active descriptor in interpreter state;
2. mirror the low byte of `D[2]` into system slot `0xD5`;
3. commit the cursor move.

`WPRINTV` shall operate in the following order:

1. read the source bound from `S[0]`;
2. compute the requested logical end column `D[0] + N`;
3. clip on the left against `G[0]` and on the right against `G[0] + G[4]`;
4. if left clipping occurs, advance both source offset and logical start column;
5. call `SETWIN` on the visible start position;
6. if that `SETWIN` fails, emit nothing;
7. otherwise emit the visible span of bytes from the source vector;
8. advance `D[0]` to the logical post-print column; and
9. call `SETWIN` again so that descriptor state reflects logical cursor motion even when the visible span was right-clipped.

In ordinary PC BIOS text mode, only bits `0`, `3`, and `5` of `D[2]` affect the rendered text attribute.

## Appendix A. Table of Opcodes

### A.1 General Notes

1. Base opcodes are listed in numeric order.
2. Extended opcodes are written as `0x5F xx`.
3. Packed families are described as opcode ranges.
4. Stack notation uses the convention: `$1` is the top word on the stack (last pushed), `$2` is the next word down (previously pushed), and so on.
5. Floating-point arguments are written as `real($1)`, meaning "the floating-point number at the address `$1`".
6. In the `Operands` column below, `-` means no inline operands, `B` means one byte, `W` means one word, `WW` means two words, and `J` means a mixed jump encoding that is either one byte or a byte followed by a word.
7. In the `Pops` and `Pushes` columns below, `varies` means the exact number of values popped or pushed depends on an operand, an argument from the stack, or the number of values returned by a called procedure.

### A.2 Base Opcodes `0x00-0x10`

| Opcode | Mnemonic | Operands | Pops | Pushes | Description |
| --- | --- | --- | ---: | ---: | --- |
| 0x00 | [`BREAK`](#0x00-break) | `-` | 0 | 0 | reserved debug-trap or breakpoint operation. Structural decoding is defined; full VM-visible semantics are not defined by this edition. |
| 0x01 | [`ADD`](#0x01-add) | `-` | 2 | 1 | pop two words and push their sum. |
| 0x02 | [`SUB`](#0x02-sub) | `-` | 2 | 1 | push `$2 - $1`. |
| 0x03 | [`MUL`](#0x03-mul) | `-` | 2 | 1 | pop two words and push their product. |
| 0x04 | [`DIV`](#0x04-div) | `-` | 2 | 1 | push `$2 / $1`. |
| 0x05 | [`MOD`](#0x05-mod) | `-` | 2 | 1 | push `$2 mod $1`. |
| 0x06 | [`NEG`](#0x06-neg) | `-` | 1 | 1 | push `-$1`. |
| 0x07 | [`ASHIFT`](#0x07-ashift) | `-` | 2 | 1 | push `$2` arithmetically shifted by `$1` bits. Positive counts shift left; negative counts shift right with sign extension. |
| 0x08 | [`INCL`](#0x08-incl) | `B` | 0 | 1 | increment local `$B` and push the new value. |
| 0x09 | [`PUSH8`](#0x09-push8) | `-` | 0 | 1 | push constant `8`. |
| 0x0A | [`PUSH4`](#0x0a-push4) | `-` | 0 | 1 | push constant `4`. |
| 0x0B | [`DECL`](#0x0b-decl) | `B` | 0 | 1 | decrement local `$B` and push the new value. |
| 0x0C | [`PUSHm1`](#0x0c-pushm1) | `-` | 0 | 1 | push constant `0xFFFF`. |
| 0x0D | [`PUSH3`](#0x0d-push3) | `-` | 0 | 1 | push constant `3`. |
| 0x0E | [`AND`](#0x0e-and) | `-` | 2 | 1 | bitwise AND. |
| 0x0F | [`OR`](#0x0f-or) | `-` | 2 | 1 | bitwise OR. |
| 0x10 | [`SHIFT`](#0x10-shift) | `-` | 2 | 1 | push `$2` logically shifted by `$1` bits. Positive counts shift left; negative counts shift right with zero extension. |

### A.3 Base Opcodes `0x11-0x21`

| Opcode | Mnemonic | Operands | Pops | Pushes | Description |
| --- | --- | --- | ---: | ---: | --- |
| 0x11 | [`VALLOC`](#0x11-valloc) | `-` | 1 | 1 | allocate a vector with `$1` visible words and push its handle. |
| 0x12 | [`VALLOCI`](#0x12-valloci) | `-` | varies | 1 | pop `$1 = word count`, then pop that many initializer words and allocate a vector containing them in original push order. |
| 0x13 | [`VFREE`](#0x13-vfree) | `-` | 2 | 0 | free the managed vector block identified by `$2`, using `$1` as the size in words. |
| 0x14 | [`TALLOC`](#0x14-talloc) | `-` | 1 | 1 | allocate a tuple with `$1` visible words and push its handle. |
| 0x15 | [`TALLOCI`](#0x15-talloci) | `-` | varies | 1 | pop `$1 = word count`, then pop that many initializer words and allocate a tuple containing them in original push order. |
| 0x16 | [`VLOADW`](#0x16-vloadw) | `-` | 2 | 1 | load a word from handle `$2` using dynamic 1-based index `$1`. |
| 0x17 | [`VLOADB`](#0x17-vloadb) | `-` | 2 | 1 | load a byte from handle `$2` using dynamic 1-based byte index `$1`. |
| 0x18 | [`VLOADW_`](#0x18-vloadw_) | `B` | 1 | 1 | load a word from handle `$1` using the inline zero-based slot index. |
| 0x19 | [`VLOADB_`](#0x19-vloadb_) | `B` | 1 | 1 | load a byte from handle `$1` using the inline zero-based byte index. |
| 0x1A | [`VPUTW`](#0x1a-vputw) | `-` | 3 | 0 | store `$1` into handle `$3` at dynamic 1-based word index `$2`. |
| 0x1B | [`VPUTB`](#0x1b-vputb) | `-` | 3 | 0 | store the low byte of `$1` into handle `$3` at dynamic 1-based byte index `$2`. |
| 0x1C | [`VPUTW_`](#0x1c-vputw_) | `B` | 2 | 0 | store `$1` into handle `$2` at the inline zero-based word slot. |
| 0x1D | [`VPUTB_`](#0x1d-vputb_) | `B` | 2 | 0 | store the low byte of `$1` into handle `$2` at the inline zero-based byte index. |
| 0x1E | [`VECSETW`](#0x1e-vecsetw) | `-` | 3 | 1 | fill handle `$3` with `$2` words of value `$1`, then leave that handle on the stack. |
| 0x1F | [`VECSETB`](#0x1f-vecsetb) | `-` | 3 | 1 | fill handle `$3` with `$2` bytes of value `low8($1)`, then leave that handle on the stack. |
| 0x20 | [`VECCPYW`](#0x20-veccpyw) | `-` | 3 | 1 | copy `$2` words from source handle `$3` to destination handle `$1`, then leave the destination handle on the stack. |
| 0x21 | [`VECCPYB`](#0x21-veccpyb) | `-` | 5 | 1 | copy `$4` bytes from source handle `$5` at byte offset `$2` to destination handle `$3` at byte offset `$1`, then leave the destination handle on the stack. |

### A.4 Base Opcodes `0x22-0x2F`

| Opcode | Mnemonic | Operands | Pops | Pushes | Description |
| --- | --- | --- | ---: | ---: | --- |
| 0x22 | [`LOADG`](#0x22-loadg) | `B` | 0 | 1 | load a program global by byte index. |
| 0x23 | [`LOADMG`](#0x23-loadmg) | `B` | 0 | 1 | load a module global by byte index. |
| 0x24 | [`PUSH2`](#0x24-push2) | `-` | 0 | 1 | push constant `2`. |
| 0x25 | [`PUSHW`](#0x25-pushw) | `W` | 0 | 1 | push an inline little-endian word. |
| 0x26 | [`PUSHB`](#0x26-pushb) | `B` | 0 | 1 | push an inline byte as a word-sized value. |
| 0x27 | [`PUSH_NIL`](#0x27-push_nil) | `-` | 0 | 1 | push **FALSE**. |
| 0x28 | [`PUSH0`](#0x28-push0) | `-` | 0 | 1 | push constant `0`. |
| 0x29 | [`DUP`](#0x29-dup) | `-` | 0 | 1 | duplicate the top stack word. |
| 0x2A | [`PUSHm8`](#0x2A-PUSHm8) | `-` | 0 | 1 | push constant `0xFFF8`. |
| 0x2B | [`PUSH5`](#0x2b-push5) | `-` | 0 | 1 | push constant `5`. |
| 0x2C | [`PUSH1`](#0x2c-push1) | `-` | 0 | 1 | push constant `1`. |
| 0x2D | [`PUTMG`](#0x2d-putmg) | `B` | 1 | 0 | store the top stack word into a module global by byte index. |
| 0x2E | [`PUSHFF`](#0x2e-pushff) | `-` | 0 | 1 | push constant `0x00FF`. |
| 0x2F | [`POP`](#0x2f-pop) | `-` | 1 | 0 | discard the top stack word. |

### A.5 Base Opcodes `0x30-0x3E`

| Opcode | Mnemonic | Operands | Pops | Pushes | Description |
| --- | --- | --- | ---: | ---: | --- |
| 0x30 | [`JUMP`](#0x30-jump) | J | 0 | 0 | unconditional branch. A nonzero inline byte denotes a signed relative jump; zero denotes an absolute 16-bit target in the following word. |
| 0x31 | [`JUMPZ`](#0x31-jumpz) | J | 1 | 0 | jump if `$1 == 0`. |
| 0x32 | [`JUMPNZ`](#0x32-jumpnz) | J | 1 | 0 | jump if `$1 != 0`. |
| 0x33 | [`JUMPF`](#0x33-jumpf) | J | 1 | 0 | jump if `$1 == FALSE`. |
| 0x34 | [`JUMPNF`](#0x34-jumpnf) | J | 1 | 0 | jump if `$1 != FALSE`. |
| 0x35 | [`JUMPGZ`](#0x35-jumpgz) | J | 1 | 0 | jump if `$1 > 0`. |
| 0x36 | [`JUMPLEZ`](#0x36-jumplez) | J | 1 | 0 | jump if `$1 <= 0`. |
| 0x37 | [`JUMPLZ`](#0x37-jumplz) | J | 1 | 0 | jump if `$1 < 0`. |
| 0x38 | [`JUMPGEZ`](#0x38-jumpgez) | J | 1 | 0 | jump if `$1 >= 0`. |
| 0x39 | [`JUMPL`](#0x39-jumpl) | J | 2 | 0 | jump if `$2 < $1`. |
| 0x3A | [`JUMPLE`](#0x3a-jumple) | J | 2 | 0 | jump if `$2 <= $1`. |
| 0x3B | [`JUMPGE`](#0x3b-jumpge) | J | 2 | 0 | jump if `$2 >= $1`. |
| 0x3C | [`JUMPG`](#0x3c-jumpg) | J | 2 | 0 | jump if `$2 > $1`. |
| 0x3D | [`JUMPEQ`](#0x3d-jumpeq) | J | 2 | 0 | jump if `$2 == $1` |
| 0x3E | [`JUMPNE`](#0x3e-jumpne) | J | 2 | 0 | jump if `$2 != $1` |

### A.6 Base Opcodes `0x3F-0x4F`

| Opcode | Mnemonic | Operands | Pops | Pushes | Description |
| --- | --- | --- | ---: | ---: | --- |
| 0x3F | [`CALL0`](#0x3f-call0) | `W` | 0 | varies | near call with zero explicit arguments. |
| 0x40 | [`CALL1`](#0x40-call1) | `W` | 1 | varies | near call with one explicit argument. |
| 0x41 | [`CALL2`](#0x41-call2) | `W` | 2 | varies | near call with two explicit arguments. |
| 0x42 | [`CALL3`](#0x42-call3) | `W` | 3 | varies | near call with three explicit arguments. |
| 0x43 | [`CALL`](#0x43-call) | `B` | varies | varies | computed near call; inline byte gives argument count. |
| 0x44 | [`CALLF0`](#0x44-callf0) | `W` | 0 | varies | far call with zero explicit arguments. |
| 0x45 | [`CALLF1`](#0x45-callf1) | `W` | 1 | varies | far call with one explicit argument. |
| 0x46 | [`CALLF2`](#0x46-callf2) | `W` | 2 | varies | far call with two explicit arguments. |
| 0x47 | [`CALLF3`](#0x47-callf3) | `W` | 3 | varies | far call with three explicit arguments. |
| 0x48 | [`CALLF`](#0x48-callf) | `B` | varies | varies | computed far call; inline byte gives argument count. |
| 0x49 | [`RETURN`](#0x49-return) | `-` | 1 | 1 | return `$1` to the caller as a one-word result. |
| 0x4A | [`RFALSE`](#0x4a-rfalse) | `-` | 0 | 1 | return **FALSE**. |
| 0x4B | [`RZERO`](#0x4b-rzero) | `-` | 0 | 1 | return `0`. |
| 0x4C | [`PUSH6`](#0x4c-push6) | `-` | 0 | 1 | push constant `6`. |
| 0x4D | [`HALT`](#0x4d-halt) | `W` | 0 | 0 | terminate execution with an inline word argument. |
| 0x4E | [`NEXTB`](#0x4e-nextb) | `-` | 0 | 0 | advance to the next 256-byte code block. |
| 0x4F | [`PUSH7`](#0x4f-push7) | `-` | 0 | 1 | push constant `7`. |

### A.7 Base Opcodes `0x50-0x5E`

| Opcode | Mnemonic | Operands | Pops | Pushes | Description |
| --- | --- | --- | ---: | ---: | --- |
| 0x50 | [`PRINTV`](#0x50-printv) | `-` | 3 | 0 | print bytes from a VM vector or address range. Broad output role is defined; exact addressing convention is reserved by this edition. |
| 0x51 | [`LOADVB2`](#0x51-loadvb2) | `-` | 2 | 1 | load a raw byte from handle `$2` using alternate 1-based byte index `$1`. |
| 0x52 | [`PUTVB2`](#0x52-putvb2) | `-` | 3 | 0 | store the low byte of `$1` into handle `$3` using the alternate 1-based byte index `$2`. |
| 0x53 | [`REST`](#0x53-rest) | `-` | 2 | 1 | alias encoding of `ADD`. |
| 0x54 | [`INCLV`](#0x54-inclv) | `B` | 0 | 0 | increment a local selected by inline byte operand without pushing the new value. Dereferencing **FALSE** traps. |
| 0x55 | [`RET`](#0x55-ret) | `-` | 0 | 0 | return no pushed result. |
| 0x56 | [`PUTL`](#0x56-putl) | `B` | 1 | 0 | store the top stack word into local `$B`. |
| 0x57 | [`PUSHL`](#0x57-pushl) | `B` | 0 | 1 | push local `$B`. |
| 0x58 | [`STOREL`](#0x58-storel) | `B` | 0 | 0 | copy the top stack word into local `$B` while preserving it on the stack. |
| 0x59 | [`BITSVL`](#0x59-bitsvl) | `W` | 0 | 1 | extract a bitfield from a vector word using an inline control word and a vector handle held in a local. |
| 0x5A | [`BITSV`](#0x5a-bitsv) | `W` | 1 | 1 | extract a bitfield from a vector word using an inline control word and a vector handle from the stack. |
| 0x5B | [`BBSETVL`](#0x5b-bbsetvl) | `W` | 1 | 0 | replace a multi-bit field inside a vector word using a local-held vector handle. |
| 0x5C | [`BBSETV`](#0x5c-bbsetv) | `W` | 2 | 0 | replace a multi-bit field inside a vector word using a stack-held vector handle. |
| 0x5D | [`BSETVL`](#0x5d-bsetvl) | `W` | 0 | 0 | set or clear one bit in a vector word using a local-held vector handle. |
| 0x5E | [`BSETV`](#0x5e-bsetv) | `W` | 1 | 0 | set or clear one bit in a vector word using a stack-held vector handle. |

### A.8 Extended Opcodes `0x5F xx`

| Opcode | Mnemonic | Operands | Pops | Pushes | Description |
| --- | --- | --- | ---: | ---: | --- |
| 0x5F 0x01 | [`XOR`](#0x5f-0x01-xor) | `-` | 2 | 1 | bitwise XOR. |
| 0x5F 0x02 | [`NOT`](#0x5f-0x02-not) | `-` | 1 | 1 | bitwise complement. |
| 0x5F 0x03 | [`ROTATE`](#0x5f-0x03-rotate) | `-` | 2 | 1 | rotate `$2` by `$1` bit positions and push the rotated word. |
| 0x5F 0x04 | [`VFIND`](#0x5f-0x04-vfind) | `-` | 3 | 1 | vector search operation returning an index or **FALSE**. |
| 0x5F 0x05 | [`STRCHR`](#0x5f-0x05-strchr) | `-` | 2 | 1 | search VM string handle `$1` for character `low8($2)` and return the found index or **FALSE**. |
| 0x5F 0x06 | [`PUTG`](#0x5f-0x06-putg) | `B` | 1 | 0 | store into a program global by byte index. |
| 0x5F 0x07 | [`POPN`](#0x5f-0x07-popn) | `-` | varies | 0 | variable-sized stack drop. |
| 0x5F 0x09 | [`LONGJMPR`](#0x5f-0x09-longjmpr) | `-` | 2 | 1 | non-local jump through saved state `$2`, returning `$1` at the restored landing point. |
| 0x5F 0x0A | [`LONGJMP`](#0x5f-0x0a-longjmp) | `-` | 1 | 0 | non-local jump through saved state `$1` with no explicit return value. |
| 0x5F 0x0B | [`SETJMP`](#0x5f-0x0b-setjmp) | `WW` | 0 | 1 | save non-local control state. |
| 0x5F 0x0C | [`OPEN`](#0x5f-0x0c-open) | `B` | 0 | 1 | open or attach a logical channel. The inline byte is the mode bitfield defined in section 8.3. |
| 0x5F 0x0D | [`CLOSE`](#0x5f-0x0d-close) | `-` | 1 | 1 | close a channel-like value and return a status result. |
| 0x5F 0x0E | [`READ`](#0x5f-0x0e-read) | `-` | 3 | 1 | read `$1` words from channel `$2` into vector `$3`, beginning at visible word slot `1`. |
| 0x5F 0x0F | [`WRITE`](#0x5f-0x0f-write) | `-` | 3 | 1 | write `$1` words from vector `$3` to channel `$2`, beginning at visible word slot `1`. |
| 0x5F 0x10 | [`READREC`](#0x5f-0x10-readrec) | `-` | 4 | 1 | read `$1` records from channel `$3` starting at record `$2` into the byte payload of vector `$4`. |
| 0x5F 0x11 | [`WRITEREC`](#0x5f-0x11-writerec) | `-` | 4 | 1 | write `$1` records from the byte payload of vector `$4` to channel `$3` starting at record `$2`. |
| 0x5F 0x12 | [`DISP`](#0x5f-0x12-disp) | `B` | 0 | 0 | display-control suboperation selected by inline byte. |
| 0x5F 0x13 | [`XDISP`](#0x5f-0x13-xdisp) | `B` | 1 | 1 | extended display-control suboperation selected by inline byte. |
| 0x5F 0x14 | [`FSIZE`](#0x5f-0x14-fsize) | `-` | 1 | 1 | file size query in block-like units. |
| 0x5F 0x15 | [`UNLINK`](#0x5f-0x15-unlink) | `-` | 1 | 1 | delete a named file. |
| 0x5F 0x16 | [`POPRET`](#0x5f-0x16-popret) | `-` | varies | 0 | discard the just-returned result bundle. |
| 0x5F 0x17 | [`KBINPUT`](#0x5f-0x17-kbinput) | `-` | 0 | 1 | nonblocking keyboard poll. |
| 0x5F 0x18 | [`FADD`](#0x5f-0x18-fadd) | `-` | 3 | 1 | floating-point addition over 8-byte real numbers. |
| 0x5F 0x19 | [`FSUB`](#0x5f-0x19-fsub) | `-` | 3 | 1 | floating-point subtraction over the same format (`real($2) - real($1)`). |
| 0x5F 0x1A | [`FMUL`](#0x5f-0x1a-fmul) | `-` | 3 | 1 | floating-point multiplication over the same format. |
| 0x5F 0x1B | [`FDIV`](#0x5f-0x1b-fdiv) | `-` | 3 | 1 | floating-point division over the same format (`real($2) / real($1)`). |
| 0x5F 0x1C | [`TPOP`](#0x5f-0x1c-tpop) | `-` | 1 | 0 | move the tuple-stack pointer upward by `$1` words. |
| 0x5F 0x1D | [`FLOG`](#0x5f-0x1d-flog) | `-` | 2 | 1 | natural logarithm over 8-byte real numbers. |
| 0x5F 0x1E | [`FEXP`](#0x5f-0x1e-fexp) | `-` | 2 | 1 | exponential (inverse natural logarithm) over 8-byte real numbers. |
| 0x5F 0x1F | [`STRICMP`](#0x5f-0x1f-stricmp) | `-` | 5 | 1 | compare first string slice `($4, $2, $1)` against second string `($5, 0, $3)` case-insensitively without skipping spaces; return `+1`, `0`, or `-1` with the VM's inverted sign convention. |
| 0x5F 0x20 | [`STRICMP1`](#0x5f-0x20-stricmp1) | `-` | 5 | 1 | perform `STRICMP`, but use first string handle `$4 - 1` instead of `$4`. |
| 0x5F 0x21 | [`WPRINTV`](#0x5f-0x21-wprintv) | `-` | 4 | 1 | clipped window print with `$4 = D`, `$3 = S`, `$2 = N`, and `$1 = O`. |
| 0x5F 0x22 | [`SETWIN`](#0x5f-0x22-setwin) | `-` | 1 | 1 | activate the display window described by descriptor handle `$1`. |
| 0x5F 0x23 | [`KEYCMP`](#0x5f-0x23-keycmp) | `-` | 2 | 1 | compare structured sort keys. |
| 0x5F 0x24 | [`MEMCMP`](#0x5f-0x24-memcmp) | `-` | 3 | 1 | compare `$1` bytes of aggregate `$2` against aggregate `$3` and push the VM comparison result. |
| 0x5F 0x25 | [`MEMCMPO`](#0x5f-0x25-memcmpo) | `-` | 4 | 1 | compare `$2` bytes of aggregate `$3`, starting at raw byte offset `$1`, against the payload of aggregate `$4`, then push the VM comparison result. |
| 0x5F 0x26 | [`ADVANCE`](#0x5f-0x26-advance) | `-` | 2 | 1 | vector helper deriving an offset or pointer from an indexed byte. (used to advance to the next B-tree node) |
| 0x5F 0x27 | [`DECMG`](#0x5f-0x27-decmg) | `B` | 0 | 1 | decrement a module global and push the new value. |
| 0x5F 0x28 | [`DECG`](#0x5f-0x28-decg) | `B` | 0 | 1 | decrement a program global and push the new value. |
| 0x5F 0x29 | [`POPI`](#0x5f-0x29-popi) | `B` | varies | 0 | discard `$B` words from the stack. |
| 0x5F 0x2A | [`INCG`](#0x5f-0x2a-incg) | `B` | 0 | 1 | increment a program global and push the new value. |
| 0x5F 0x2B | [`INCMG`](#0x5f-0x2b-incmg) | `B` | 0 | 1 | increment a module global and push the new value. |
| 0x5F 0x2C | [`STOREMG`](#0x5f-0x2c-storemg) | `B` | 0 | 0 | store into a module global while preserving the top of stack. |
| 0x5F 0x2D | [`STOREG`](#0x5f-0x2d-storeg) | `B` | 0 | 0 | store into a program global while preserving the top of stack. |
| 0x5F 0x2E | [`RETN`](#0x5f-0x2e-retn) | `B` | varies | varies | counted return; inline byte gives the number of returned words. |
| 0x5F 0x2F | [`PRCHAR`](#0x5f-0x2f-prchar) | `-` | 1 | 0 | print one character without character-set translation. |
| 0x5F 0x30 | [`UNPACK`](#0x5f-0x30-unpack) | `-` | 4 | 1 | unpack packed 5-bit text symbols with `$4 = source vector`, `$3 = destination vector`, `$2 = word count`, and `$1 = destination byte offset`. |
| 0x5F 0x31 | [`PINM`](#0x5f-0x31-pinm) | `-` | 0 | 0 | preload and pin all code pages in current module |
| 0x5F 0x32 | [`UNPINM`](#0x5f-0x32-unpinm) | `-` | 0 | 0 | unpin current module's code pages |
| 0x5F 0x34 | [`FMTREAL`](#0x5f-0x34-fmtreal) | `-` | 4 | 1 | format 8-byte real number as a string. |
| 0x5F 0x35 | [`PRSREAL`](#0x5f-0x35-prsreal) | `-` | 6 | 1 | parse a string as an 8-byte real number |
| 0x5F 0x36 | [`LOOKUP`](#0x5f-0x36-lookup) | `-` | 1 | 1 | resolve packed descriptor key `$1` into a descriptor-vector handle. |
| 0x5F 0x37 | [`EXTRACT`](#0x5f-0x37-extract) | `-` | 8 | 1 | packed descriptor-field walker with `$1` as the mode or subfield selector and `$2-$8` as the descriptor-walker parameters summarized in A.9.3. |
| 0x5F 0x38 | [`RENAME`](#0x5f-0x38-rename) | `-` | 2 | 1 | rename a file from one string name to another. |

### A.9 Extended Text and Descriptor Helpers

#### A.9.1 `UNPACK` (`0x5F 0x30`)

`UNPACK` has the following stack inputs:

* `$4 = packed source vector`;
* `$3 = destination vector`;
* `$2 = word count`;
* `$1 = destination byte offset`.

Its behavior is:

1. read 16-bit source words;
2. extract three 5-bit symbols from each source word by rotate-and-mask;
3. write each extracted symbol as one byte into the destination vector;
4. if the source word high bit is set, OR `0x80` into the last emitted symbol and return **FALSE**;
5. otherwise return the destination offset after the last emitted symbol.

`UNPACK` performs only raw 5-bit symbol extraction. Alphabet mapping is bytecode-level logic and not part of the hardwired interpreter contract.

#### A.9.2 `LOOKUP`

`LOOKUP` interprets its operand as a packed descriptor key.

It shall:

1. consult a four-word root vector held in system slot `0xD8`;
2. use the low seven bits as a primary 1-based index;
3. use the high byte as a secondary 1-based index;
4. return a descriptor-vector handle directly if found;
5. otherwise fall back to a resolver procedure selected by `D8[3]`.

#### A.9.3 `EXTRACT`

`EXTRACT` is a field walker and extractor over packed descriptor records.

Its effective stack effect is `8 -> 1`.

The last stack word is a mode or subfield selector. Earlier operands supply a base descriptor pointer, span, optional destination buffer, optional tag selector, optional skip count, initial word offset, and optional nonzero-content guard.

Mode semantics are:

* mode **FALSE**: operate on the whole selected item, returning or copying its byte span;
* mode `0`: return the record header high byte;
* mode `n > 0`: select the `n`th packed subfield of a compound record.

### A.10 Packed Opcode Families

| Opcode | Mnemonic | Operands | Pops | Pushes | Description |
| --- | --- | --- | ---: | ---: | --- |
| 0x60-0x9F | [`VREAD_loc_vec`](#0x60-0x9F-VREAD-loc-vec) | `-` | 0 | 1 | compact vector-word read. Bits `0-3` select a local containing the vector handle; bits `4-5` select one of the first four visible words. |
| 0xA0-0xBF | [`PUSH_Ln`](#0xA0-0xBF-PUSH-Ln) | `-` | 0 | 1 | compact local push for locals `0..31`. |
| 0xC0-0xDF | [`PUT_Ln`](#0xC0-0xDF-PUT-Ln) | `-` | 1 | 0 | compact local store for locals `0..31`, consuming the top of stack. |
| 0xE0-0xFF | [`STORE_Ln`](#0xE0-0xFF-STORE-Ln) | `-` | 0 | 0 | compact local store for locals `0..31`, preserving the top of stack. |

## Appendix B. Opcode Reference

### `0x00 BREAK`

Operands: -
Pop/Push: 0/0

reserved debug-trap or breakpoint operation. Structural decoding is defined; full VM-visible semantics are not defined by this edition.

### `0x01 ADD`

Operands: -
Pop/Push: 2/1

Integer addition: `$2 + $1`.

### `0x02 SUB`

Operands: -
Pop/Push: 2/1

Integer subtraction: `$2 - $1`.

### `0x03 MUL`

Operands: -
Pop/Push: 2/1

Integer multiplication: `$2 * $1`.

### `0x04 DIV`

Operands: -
Pop/Push: 2/1

Integer division: `$2 / $1`.

### `0x05 MOD`

Operands: -
Pop/Push: 2/1

Integer modulus: `$2 mod $1`.

Note, this instruction must not return a negative value. Therefore, it cannot be
implemented as a direct wrapper for the `%` operator in some common languages;
`$1` must be added to a negative result to make it positive.

### `0x06 NEG`

Operands: -
Pop/Push: 1/1

Integer negation: `-$1`.

### `0x07 ASHIFT`

Operands: -
Pop/Push: 2/1

push `$2` arithmetically shifted by `$1` bits. Positive counts shift left; negative counts shift right with sign extension.

Arithmetic shift: `$2` shifted left by `$1` bits, or right if `$1` is negative,
extending the sign bit for right shifts.

### `0x08 INCL`

Operands: B
Pop/Push: 0/1

Increment local `$B` and push the new value.

### `0x09 PUSH8`

Operands: -
Pop/Push: 0/1

Push constant `8`.

### `0x0A PUSH4`

Operands: -
Pop/Push: 0/1

Push constant `4`.

### `0x0B DECL`

Operands: B
Pop/Push: 0/1

Decrement local `$B` and push the new value.

### `0x0C PUSHm1`

Operands: -
Pop/Push: 0/1

Push constant `0xFFFF` (`-1`).

### `0x0D PUSH3`

Operands: -
Pop/Push: 0/1

Push constant `3`.

### `0x0E AND`

Operands: -
Pop/Push: 2/1

Bitwise AND: `$2 & $1`.

### `0x0F OR`

Operands: -
Pop/Push: 2/1

Bitwise OR: `$2 | $1`.

### `0x10 SHIFT`

Operands: -
Pop/Push: 2/1

Logical shift: `$2` shifted left by `$1` bits, or right if `$1` is negative,
extending with zeros for right shifts.

### `0x11 VALLOC`

Operands: -
Pop/Push: 1/1

Allocate a vector with a size of `$1` total words, and push its handle.

The precise operation of the allocator is not defined by this specification.

### `0x12 VALLOCI`

Operands: -
Pop/Push: varies/1

Pop `$1` as a word count, then pop that many initializer words and allocate a
vector containing them in original push order (first word pushed is stored at
index 1).

The precise operation of the allocator is not defined by this specification.

### `0x13 VFREE`

Operands: -
Pop/Push: 2/0

Free the vector block identified by `$2`, using `$1` as the size in words. `$2`
must have been previously allocated by `VALLOC` or `VALLOCI`, and `$1` must
match the word count used for allocation.

The precise operation of the allocator is not defined by this specification.

### `0x14 TALLOC`

Operands: -
Pop/Push: 1/1

Allocate a tuple with `$1` visible words and push its handle.

### `0x15 TALLOCI`

Operands: -
Pop/Push: varies/1

Pop `$1` as a word count, then pop that many initializer words and allocate a
tuple containing them in original push order (first word pushed is stored at
index 1).

### `0x16 VLOADW`

Operands: -
Pop/Push: 2/1

Load a word from handle `$2` using 1-based word index `$1 + 1` (i.e., skipping
an initial length word).

### `0x17 VLOADB`

Operands: -
Pop/Push: 2/1

Load a byte from handle `$2` using 1-based word index `$1 + 2` (i.e., skipping
an initial length word).

### `0x18 VLOADW_`

Operands: B
Pop/Push: 1/1

Load a word from handle `$1` using the inline zero-based word offset `$B`.

### `0x19 VLOADB_`

Operands: B
Pop/Push: 1/1

Load a byte from handle `$1` using the inline zero-based byte offset `$B`.

### `0x1A VPUTW`

Operands: -
Pop/Push: 3/0

Store `$1` into handle `$3` at 1-based word index `$2 + 1` (i.e., skipping an
initial length word),

### `0x1B VPUTB`

Operands: -
Pop/Push: 3/0

Store the low byte of `$1` into handle `$3` at 1-based byte index `$2 + 2`
(i.e., skipping an initial length word).

### `0x1C VPUTW_`

Operands: B
Pop/Push: 2/0

Store `$1` into handle `$2` at the inline zero-based word offset `$B`.

### `0x1D VPUTB_`

Operands: B
Pop/Push: 2/0

Store the low byte of `$1` into handle `$2` at the inline zero-based byte offset
`$B`.

### `0x1E VECSETW`

Operands: -
Pop/Push: 3/1

Fill handle `$3` with `$2` copies of the word `$1` (skipping an initial length
word), then leave that handle on the stack.

### `0x1F VECSETB`

Operands: -
Pop/Push: 3/1

Fill handle `$3` with `$2` copies of the low byte of `$1` (skipping an initial
length word), then leave that handle on the stack.

### `0x20 VECCPYW`

Operands: -
Pop/Push: 3/1

Copy `$2` words from source handle `$3` to destination handle `$1` (skipping an
initial length word), then leave the destination handle on the stack.

### `0x21 VECCPYB`

Operands: -
Pop/Push: 5/1

Copy `$4` bytes from source handle `$5` at zero-based byte offset `$2` (skipping
an initial length word) to destination handle `$3` at zero-based byte offset
`$1`, then leave the destination handle on the stack.

### `0x22 LOADG`

Operands: B
Pop/Push: 0/1

Push the value of program global variable `$B`.

### `0x23 LOADMG`

Operands: B
Pop/Push: 0/1

Push the value of module global variable or system variable `$B`.

### `0x24 PUSH2`

Operands: -
Pop/Push: 0/1

Push constant `2`.

### `0x25 PUSHW`

Operands: W
Pop/Push: 0/1

Push the inline word value `$W`.

### `0x26 PUSHB`

Operands: B
Pop/Push: 0/1

Push the inline byte value `$B`, with sign extension. (???)

### `0x27 PUSH_NIL`

Operands: -
Pop/Push: 0/1

Push **FALSE** (`0x8001`).

### `0x28 PUSH0`

Operands: -
Pop/Push: 0/1

Push constant `0`.

### `0x29 DUP`

Operands: -
Pop/Push: 0/1

Duplicate the top stack word.

### `0x2A PUSHm8`

Operands: -
Pop/Push: 0/1

Push constant `0xFFF8` (`-8`).

### `0x2B PUSH5`

Operands: -
Pop/Push: 0/1

Push constant `5`.

### `0x2C PUSH1`

Operands: -
Pop/Push: 0/1

Push constant `1`.

### `0x2D PUTMG`

Operands: B
Pop/Push: 1/0

Store `$1` into the module global variable or system variable `$B`.

### `0x2E PUSHFF`

Operands: -
Pop/Push: 0/1

Push constant `0x00FF` (`255`).

### `0x2F POP`

Operands: -
Pop/Push: 1/0

Discard the top stack word.

### `0x3F CALL0`

Operands: W
Pop/Push: 0/varies

Near call to code offset `$W` in the current module, with zero arguments.

### `0x40 CALL1`

Operands: W
Pop/Push: 1/varies

Near call to code offset `$W` in the current module, with one argument popped
from the stack.

### `0x41 CALL2`

Operands: W
Pop/Push: 2/varies

Near call to code offset `$W` in the current module, with two arguments popped
from the stack.

### `0x42 CALL3`

Operands: W
Pop/Push: 3/varies

Near call to code offset `$W` in the current module, with three arguments popped
from the stack.

### `0x43 CALL`

Operands: B
Pop/Push: varies/varies

Near call to code offset `$1` in the current module, with `$B` arguments popped
from the stack.

### `0x44 CALLF0`

Operands: W
Pop/Push: 0/varies

Far call to selector `$W`, with zero arguments.

### `0x45 CALLF1`

Operands: W
Pop/Push: 1/varies

Far call to selector `$W`, with one argument popped from the stack.

### `0x46 CALLF2`

Operands: W
Pop/Push: 2/varies

Far call to selector `$W`, with two arguments popped from the stack.

### `0x47 CALLF3`

Operands: W
Pop/Push: 3/varies

Far call to selector `$W`, with three arguments popped from the stack.

### `0x48 CALLF`

Operands: B
Pop/Push: varies/varies

Far call to selector `$1`, with `$B` arguments popped from the stack.

### `0x49 RETURN`

Operands: -
Pop/Push: 1/1

Return `$1` to the caller as a one-word result.

### `0x4A RFALSE`

Operands: -
Pop/Push: 0/1

Return **FALSE** to the caller as a one-word result.

### `0x4B RZERO`

Operands: -
Pop/Push: 0/1

Return `0` to the caller as a one-word result.

### `0x4C PUSH6`

Operands: -
Pop/Push: 0/1

Push constant `6`.

### `0x4D HALT`

Operands: W
Pop/Push: 0/0

Terminate execution with halt code `$W`. Code `1` means normal termination; any
other code means abnormal termination.

### `0x4E NEXTB`

Operands: -
Pop/Push: 0/0

Advance to the next 256-byte code block. Any further instructions in the current
block are skipped.

### `0x4F PUSH7`

Operands: -
Pop/Push: 0/1

Push constant `7`.

### `0x50 PRINTV`

Operands: -
Pop/Push: 3/0

Print bytes from a VM vector or address range. (???)

### `0x51 LOADVB2`

Operands: -
Pop/Push: 2/1

load a raw byte from handle `$2` using alternate 1-based byte index `$1`.

### `0x52 PUTVB2`

Operands: -
Pop/Push: 3/0

store the low byte of `$1` into handle `$3` using the alternate 1-based byte index `$2`.

### `0x53 REST`

Operands: -
Pop/Push: 2/1

Identical to `ADD`: push `$2 + $1`.

### `0x54 INCLV`

Operands: B
Pop/Push: 0/0

Increment the local variable `$B`. The value before incrementing must not be
FALSE (`0x8001`).

### `0x55 RET`

Operands: -
Pop/Push: 0/0

Return to the caller without pushing a result.

### `0x56 PUTL`

Operands: B
Pop/Push: 1/0

Store the top stack word into local `$B`.

### `0x57 PUSHL`

Operands: B
Pop/Push: 0/1

Push the value of local `$B`.

### `0x58 STOREL`

Operands: B
Pop/Push: 0/0

Copy the top stack word into local `$B` while preserving it on the stack.

### `0x59 BITSVL`

Operands: W
Pop/Push: 0/1

Extract a bitfield from a vector word using an inline control word and a vector handle held in a local. (???)

### `0x5A BITSV`

Operands: W
Pop/Push: 1/1

Extract a bitfield from a vector word using an inline control word and a vector handle from the stack. (???)

### `0x5B BBSETVL`

Operands: W
Pop/Push: 1/0

Replace a multi-bit field inside a vector word using a local-held vector handle. (???)

### `0x5C BBSETV`

Operands: W
Pop/Push: 2/0

Replace a multi-bit field inside a vector word using a stack-held vector handle. (???)

### `0x5D BSETVL`

Operands: W
Pop/Push: 0/0

Set or clear one bit in a vector word using a local-held vector handle. (???)

### `0x5E BSETV`

Operands: W
Pop/Push: 1/0

Set or clear one bit in a vector word using a stack-held vector handle. (???)

### `0x5F 0x01 XOR`

Operands: -
Pop/Push: 2/1

Bitwise XOR: push `$2 ^ $1`.

### `0x5F 0x02 NOT`

Operands: -
Pop/Push: 1/1

Bitwise NOT: push `~$1`.

### `0x5F 0x03 ROTATE`

Operands: -
Pop/Push: 2/1

Rotate `$2` by `$1` bit positions to the right, or to the left if `$1` is negative. The rotated bits appear in the same order at the other end of the word.

### `0x5F 0x04 VFIND`

Operands: -
Pop/Push: 3/1

Vector search operation returning an index or **FALSE**. (???)

### `0x5F 0x05 STRCHR`

Operands: -
Pop/Push: 2/1

Search VM string handle `$1` for a byte matching the low byte of `$2`, and push the found index or **FALSE** if not found.

### `0x5F 0x06 PUTG`

Operands: B
Pop/Push: 1/0

Store the top stack word into global variable `$B`.

### `0x5F 0x07 POPN`

Operands: -
Pop/Push: varies/0

Pop `$1` from the stack as a count, then discard that number of additional words from the stack.

### `0x5F 0x09 LONGJMPR`

Operands: -
Pop/Push: 2/1

Transfer control to the second address associated with activation token `$2`, pushing `$1` as a result. The state stored in the activation record is restored, and the token is invalidated.

### `0x5F 0x0A LONGJMP`

Operands: -
Pop/Push: 1/0

Transfer control to the first address associated with activation token `$1` without pushing a result. The state stored in the activation record is restored, and the token is invalidated.

### `0x5F 0x0B SETJMP`

Operands: WW
Pop/Push: 0/1

Save the current state into an activation record, as described in [section 4.8](#48-non-local-control-transfer), and push the activation token. The two inline word operands are interpreted as code offsets within the current module. The first operand identifies a target for `LONGJMP`; the second identifies a target for `LONGJUMPR`.

### `0x5F 0x0C OPEN`

Operands: B
Pop/Push: 2/1

Open or attach a channel, using the file or device name string `$2`. The inline operand is the mode byte described in [section 8.1.1](#811-open-mode-byte).

The meaning of `$1` is not defined by this specification. (???)

### `0x5F 0x0D CLOSE`

Operands: -
Pop/Push: 1/1

Close a channel and push a status result. (???)

### `0x5F 0x0E READ`

Operands: -
Pop/Push: 3/1

Read `$1` words from channel `$2` into aggregate `$3`, starting at the beginning of the aggregate.

### `0x5F 0x0F WRITE`

Operands: -
Pop/Push: 3/1

Write `$1` bytes from aggregate `$3` to channel `$2`, starting at the beginning of the aggregate.

The aggregate must contain `$1` words; only the low byte of each word is written.

### `0x5F 0x10 READREC`

Operands: -
Pop/Push: 4/1

read `$1` records from channel `$3` starting at record `$2` into the byte payload of vector `$4`.

### `0x5F 0x11 WRITEREC`

Operands: -
Pop/Push: 4/1

write `$1` records from the byte payload of vector `$4` to channel `$3` starting at record `$2`.

### `0x5F 0x12 DISP`

Operands: B
Pop/Push: 0/0

Perform the display operation selected by `$B`:
* 0: Cursor right
* 1: Cursor left
* 2: Cursor down
* 3: Cursor up
* 4: Erase to end of line
* 5: Clear from cursor row to bottom

### `0x5F 0x13 XDISP`

Operands: B
Pop/Push: 1/1

Perform the display operation selected by `$B`:
* 0: Scroll area down by `$1` lines. The area starts at the cursor row and ends at the row in system variable `0xC4`.
* 1: Scroll area up by `$1` lines. The area starts at the cursor row and ends at the row in system variable `0xC4`.
* 2/3: No effect.
* 4: Scroll area down by `$1` lines. The area starts at the row in system variable `0xC5` and ends at the row in system variable `0xC4`.
* 5: Scroll area up by `$1` lines. The area starts at the row in system variable `0xC5` and ends at the row in system variable `0xC4`.
* 6: Draw a horizontal line `$1` characters wide, starting at the cursor.

`$1` is then pushed back onto the stack.

### `0x5F 0x14 FSIZE`

Operands: -
Pop/Push: 1/1

Push the size of the file indicated by open channel `$1`, divided by 256.

### `0x5F 0x15 UNLINK`

Operands: -
Pop/Push: 1/1

Delete the file whose name is the string `$1` and close any open channels to it.

Push `0` on success or `0x8001` on failure.

### `0x5F 0x16 POPRET`

Operands: -
Pop/Push: varies/0

Discard a number of words from the stack equal to the number of words returned by the most recent `RETURN`, `RFALSE`, `RZERO`, `RET`, or `RETN` instruction.

### `0x5F 0x17 KBINPUT`

Operands: -
Pop/Push: 0/1

Poll the keyboard for input, and push a key code if a key is available, or `0x8001` if no key is available.

If the value of system variable `0xC0` is nonzero, key input is ignored, and this instruction will always push `0x8001`.

When a special key is pressed, it is exposed to the bytecode program as two key codes that must be read with two separate calls to `KBINPUT`, as described in [section 8.3](#83-keyboard-polling).

### `0x5F 0x18 FADD`

Operands: -
Pop/Push: 3/1

Floating-point addition: write `real($3) + real($2)` into `real($1)`, then push `$1`.

### `0x5F 0x19 FSUB`

Operands: -
Pop/Push: 3/1

Floating-point subtraction: write `real($3) - real($2)` into `real($1)`, then push `$1`.

### `0x5F 0x1A FMUL`

Operands: -
Pop/Push: 3/1

Floating-point multiplication: write `real($3) * real($2)` into `real($1)`, then push `$1`.

### `0x5F 0x1B FDIV`

Operands: -
Pop/Push: 3/1

Floating-point division: write `real($3) / real($2)` into `real($1)`, then push `$1`.

### `0x5F 0x1C TPOP`

Operands: -
Pop/Push: 1/0

Discard `$1` words from the tuple stack, by incrementing the internal tuple-stack pointer.

### `0x5F 0x1D FLOG`

Operands: -
Pop/Push: 2/1

Floating-point natural logarithm: write `ln(real($2))` into `real($1)`, then push `$1`.

### `0x5F 0x1E FEXP`

Operands: -
Pop/Push: 2/1

Floating-point exponential: write $1+2$ `e^real($2)` into `real($1)`, then push `$1`.

### `0x5F 0x1F STRICMP`

Operands: -
Pop/Push: 5/1

compare first string slice `($4, $2, $1)` against second string `($5, 0, $3)` case-insensitively without skipping spaces; return `+1`, `0`, or `-1` with the VM's inverted sign convention.

### `0x5F 0x20 STRICMP1`

Operands: -
Pop/Push: 5/1

perform `STRICMP`, but use first string handle `$4 - 1` instead of `$4`.

### `0x5F 0x21 WPRINTV`

Operands: -
Pop/Push: 4/1

clipped window print with `$4 = D`, `$3 = S`, `$2 = N`, and `$1 = O`.

### `0x5F 0x22 SETWIN`

Operands: -
Pop/Push: 1/1

activate the display window described by descriptor handle `$1`.

### `0x5F 0x23 KEYCMP`

Operands: -
Pop/Push: 2/1

Compare the structured sort keys at `$2` and `$1`, and push `1` if the former is greater than or equal to the latter, or `0` otherwise.

The structure of the sort keys is:
- One tiebreaker byte `C`,
- One byte giving a total number of bytes `N`,
- Two tiebreaker bytes `B` and `A`, and finally
- `N` bytes containing a series of fields.

Each field consists of a length byte `L` followed by `L` bytes of data.

The key comparison process is:
1. Starting with the first field in each key, compare each byte of the field. If any byte differs, treat the key with the lower byte as smaller and stop.
2. If all bytes match but one key's field is shorter than the other's, treat the shorter one as smaller and stop.
3. If all bytes match and the fields are the same length, and both keys have more fields remaining, move on to the next field and repeat from step 2.
4. If one key runs out of fields before the other, treat that one as smaller and stop.
5. Examine bit 6 of tiebreaker byte `C` in the first key. If the bit is clear, treat the keys as equal and stop.
6. Calculate a first tiebreaker value `((A & 0x3F) << 8) + B` from the `A` and `B` tiebreaker bytes of each key. If the values differ, treat the key with the lower value as smaller and stop.
7. Calculate a second tiebreaker value `C & 0x3F` from the `C` tiebreaker bytes of each key. If the values differ, treat the key with the lower value as smaller and stop.
8. If everything matches, treat the first key as greater and stop.

### `0x5F 0x24 MEMCMP`

Operands: -
Pop/Push: 3/1

Compare `$1` bytes of aggregate `$2` against aggregate `$3` and push the result.

The result is `1` if `$1` is *smaller* than `$2`, and `-1` if `$1` is *greater*; this is the opposite of C `memcmp`.

### `0x5F 0x25 MEMCMPO`

Operands: -
Pop/Push: 4/1

Compare `$2` bytes of aggregate `$3`, starting at byte offset `$1`, against aggregate `$4`, starting at byte offset 2, and push the result.

The result is `1` if `$1` is *smaller* than `$2`, and `-1` if `$1` is *greater*; this is the opposite of C `memcmp`.

### `0x5F 0x26 ADVANCE`

Operands: -
Pop/Push: 2/1

Read the lengths of two fields from the aggregate `$2` starting at 1-based byte index `$1`, skip over those two fields, and push the resulting 1-based index.

The aggregate is expected to contain the following at index `$1`:
* A length byte `N`,
* `N` bytes of data,
* Another length byte `M`, and
* `M` bytes of data.

The index returned is `$1 + N + M + 2`.

### `0x5F 0x27 DECMG`

Operands: B
Pop/Push: 0/1

Decrement module-level global variable `$B` and push the new value.

### `0x5F 0x28 DECG`

Operands: B
Pop/Push: 0/1

Decrement global variable `$B` and push the new value.

### `0x5F 0x29 POPI`

Operands: B
Pop/Push: varies/0

Discard `$B` words from the stack.

### `0x5F 0x2A INCG`

Operands: B
Pop/Push: 0/1

Increment global variable `$B` and push the new value.

### `0x5F 0x2B INCMG`

Operands: B
Pop/Push: 0/1

increment module-level global variable `$B` and push the new value.

### `0x5F 0x2C STOREMG`

Operands: B
Pop/Push: 1/1

Store the word at the top of the stack into module-level global variable `$B`, leaving it on top of the stack.

### `0x5F 0x2D STOREG`

Operands: B
Pop/Push: 1/1

Store the word at the top of the stack into global variable `$B`, leaving it on top of stack.

### `0x5F 0x2E RETN`

Operands: B
Pop/Push: varies/varies

Return from the current procedure, passing `$B` words from the stack back to the caller as a return value. The words are kept in the same order.

### `0x5F 0x2F PRCHAR`

Operands: -
Pop/Push: 1/0

Print the character `$1` from code page 437 at the current cursor position, then move the cursor one space to the right. If the cursor is already at the right edge of the screen, it stays there.

`$1` must be between `0` and `255`.

### `0x5F 0x30 UNPACK`

Operands: -
Pop/Push: 4/1

Unpack up to `$2` words of 5-bit symbols from aggregate `$4` into bytes in aggregate `$3`, starting at destination byte offset `$1`, and push either `0x8001` or the offset of the byte following the last destination byte.

Each source word `W` is expanded into three destination bytes: `(W >> 10) & 0x1F`, `(W >> 5) & 0x1F`, and `W & 0x1F`. Bit 15 of the source word is treated as a stop bit. If the stop bit is reached while unpacking, bit 7 is set on the final byte and `0x8001` is pushed. If no stop bit appears in the `$2` words, bit 7 is not set, and `$1 + (3 * $2)` is pushed.

This operation is reminiscent of the first step of decoding packed Z-machine text, but the interpreter doesn't perform any further translation; the unpacked values are simply zero-extended into bytes.

### `0x5F 0x34 FMTREAL`

Operands: -
Pop/Push: 4/1

Format the floating-point number in aggregate `$4` as a string into aggregate `$3`, using `$2` as precision and `$1` as flags.

If `$2` is not FALSE (`0x8001`), the number is formatted as fixed-point with exactly `$2` digits after the decimal point. No leading zero is included.

Otherwise, if bit 3 (`0x08`) of `$1` is set, the number is rounded to an integer. If bit 3 is clear, the number may be formatted as either an integer, a floating-point number with up to 15 digits after the potential decimal point, or a number in scientific notation with up to 15 significant digits, depending on its value.

### `0x5F 0x35 PRSREAL`

Operands: -
Pop/Push: 6/1

Parse a floating-point number from a string into an 8-byte real buffer, and push the destination address or FALSE.

`$6` is the source text aggregate, `$5` is the destination 8-byte real buffer,
`$4` is the source byte count, `$3` is a source offset, `$2` is an option word,
and `$1` is the address of a result word.

`PRSREAL` accepts the same numeric forms described in the Cornerstone manual: plain integers, fixed-point decimals, `$`-prefixed amounts, numbers containing comma group separators, scientific notation using `E` or `e`, a leading minus sign, and accounting-style negatives written in parentheses. Examples include `123`, `123.45`, `.99`, `$99.95`, `1,000`, `2.99e5`, `-16.5`, and `(9.95)`.

The parser stops after it finds a numeric prefix. Strings like `10%` produce a valid numeric prefix (`10`) plus trailing junk; callers may detect that case by comparing the consumed count with the supplied source length.

On success, `PRSREAL` writes the parsed real value into `real($5)`, writes the number
of consumed source characters into result byte `$1[0]`, writes a status byte
into result byte `$1[1]`, and pushes `$5`.

On failure, it still writes the consumed count into result byte `$1[0]`, writes
an error/status code into result byte `$1[1]`, and pushes `FALSE`.

The error/status codes include:

- `0x01`: normal successful parse.
- `0x02`: an exponent marker (`E` or `e`) was seen, but no exponent digits followed it.

Additional error/status codes may exist. Any code other than `0x01` indicates failure.

### `0x5F 0x36 LOOKUP`

Operands: -
Pop/Push: 1/1

resolve packed descriptor key `$1` into a descriptor-vector handle.

### `0x5F 0x37 EXTRACT`

Operands: -
Pop/Push: 8/1

packed descriptor-field walker with `$1` as the mode or subfield selector and `$2-$8` as the descriptor-walker parameters summarized in A.9.3.

### `0x5F 0x38 RENAME`

Operands: -
Pop/Push: 2/1

rename a file from one string name to another.

### `0x60-0x9F VREAD_loc_vec`

Operands: -
Pop/Push: 0/1

compact vector-word read. Bits `0-3` select a local containing the vector handle; bits `4-5` select one of the first four visible words.

### `0xA0-0xBF PUSH_Ln`

Operands: -
Pop/Push: 0/1

compact local push for locals `0..31`.

### `0xC0-0xDF PUT_Ln`

Operands: -
Pop/Push: 1/0

compact local store for locals `0..31`, consuming the top of stack.

### `0xE0-0xFF STORE_Ln`

Operands: -
Pop/Push: 0/0

compact local store for locals `0..31`, preserving the top of stack.
