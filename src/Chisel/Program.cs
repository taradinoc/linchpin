/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Globalization;
using System.Text;

namespace Chisel;

/// <summary>Entry point and shared utilities for the Chisel assembler.</summary>
internal static partial class Program
{
	// Fixed preamble words that appear at the start of every MME metadata header.
	private static readonly ushort[] HeaderPreambleWords =
	{
		0x0065,
		0x3231,
		0x3433,
		0x3635,
		0x3837,
		0x0016,
		0x00AB,
	};

	private const int HeaderWordCount = 48;
	private const int HeaderBytes = HeaderWordCount * 2;

	public static int Main(string[] args)
	{
		return CommandLineOptions.Invoke(args);
	}

	/// <summary>Assembles the source file specified by <paramref name="options"/> and writes OBJ and MME output files.</summary>
	/// <param name="options">The resolved command-line options specifying input/output paths and optional grammar path.</param>
	/// <returns>0 on success.</returns>
	private static int Assemble(CommandLineOptions options)
	{
		GrammarCatalog grammar = GrammarCatalog.Load(options.GrammarPath);
		AssemblySource source = SourceParser.Parse(options.InputPath);
		AssemblyImage image = new Assembler(grammar, source).Assemble();

		EnsureParentDirectory(options.ObjPath);
		EnsureParentDirectory(options.MmePath);
		File.WriteAllBytes(options.ObjPath, image.ObjectBytes);
		File.WriteAllBytes(options.MmePath, image.MetadataBytes);

		Console.WriteLine($"Wrote {options.ObjPath} ({image.ObjectBytes.Length} bytes)");
		Console.WriteLine($"Wrote {options.MmePath} ({image.MetadataBytes.Length} bytes)");
		Console.WriteLine($"Entry selector: module {image.EntryModuleId}, proc {image.EntryProcedureIndex}");
		Console.WriteLine($"Modules: {image.ModuleLayouts.Count}, initial RAM words: {image.InitialRamWords}");
		return 0;
	}

	/// <summary>Creates the parent directory of <paramref name="path"/> if it does not already exist.</summary>
	/// <param name="path">The file path whose parent directory should be created.</param>
	private static void EnsureParentDirectory(string path)
	{
		string? directory = Path.GetDirectoryName(path);
		if (!string.IsNullOrEmpty(directory))
		{
			Directory.CreateDirectory(directory);
		}
	}

	/// <summary>
	/// Describes a single instruction opcode from the grammar: its mnemonic, opcode byte(s), and operand schema.
	/// </summary>
	/// <param name="Mnemonic">The canonical instruction mnemonic.</param>
	/// <param name="Opcode">The primary opcode byte.</param>
	/// <param name="ExtendedOpcode">The extended opcode byte (used only when <paramref name="IsExtended"/> is <see langword="true"/>).</param>
	/// <param name="IsExtended">Whether this is a two-byte extended opcode (prefix 0x5F followed by the extended byte).</param>
	/// <param name="IsNextBlock">Whether this is the NEXTB block-continuation instruction.</param>
	/// <param name="Operands">The operand schema for this instruction.</param>
	private sealed record OpcodeInfo(string Mnemonic, byte Opcode, byte ExtendedOpcode, bool IsExtended, bool IsNextBlock, IReadOnlyList<GrammarOperand> Operands)
	{
		public int PrefixLength => IsExtended ? 2 : 1;
	}

	private sealed record ResolvedInstruction(OpcodeInfo Opcode, IReadOnlyList<string> Operands);

	private sealed record GrammarOperand(string Name, string Kind);
	private sealed record AssemblySource(
		string Path,
		IReadOnlyList<ModuleDefinition> Modules,
		IReadOnlyList<RamDirective> RamDirectives,
		IReadOnlyList<ObjDataDirective> ObjDataDirectives,
		IReadOnlyList<string> ProgramGlobals,
		IReadOnlyDictionary<string, int> ProgramGlobalNames,
		IReadOnlyDictionary<string, ConstantDefinition> Constants,
		string EntrySymbol,
		int EntryLine,
		int? ProgramGlobalsLine);

	/// <summary>A named constant definition from file-scope source.</summary>
	/// <param name="Name">The constant name.</param>
	/// <param name="ValueToken">The raw value token or expression.</param>
	/// <param name="LineNumber">The source line where this constant was defined.</param>
	private sealed record ConstantDefinition(string Name, string ValueToken, int LineNumber);

	/// <summary>Associates an exported procedure with its 0-based index in the module's procedure table.</summary>
	/// <param name="ProcedureName">The exported procedure name.</param>
	/// <param name="ExportedProcedureIndex">The 0-based procedure-table index assigned to this procedure.</param>
	/// <param name="LineNumber">The source line of the <c>.export</c> directive.</param>
	private sealed record ProcedureExportDefinition(string ProcedureName, int ExportedProcedureIndex, int LineNumber);

	/// <summary>A parsed <c>Name</c> or <c>Name=Value</c> slot declaration, as used in <c>.global</c> and <c>.local</c> directives.</summary>
	/// <param name="Name">The slot name.</param>
	/// <param name="ValueToken">The raw initializer-value token, or <see langword="null"/> if no initializer was given.</param>
	private sealed record NamedSlotDeclaration(string Name, string? ValueToken);

	/// <summary>Specifies an initial value for a single local variable slot in a procedure header.</summary>
	/// <param name="LocalIndex">The 0-based index of the local to initialize.</param>
	/// <param name="ValueToken">The raw value token or expression to evaluate as the initial value.</param>
	/// <param name="LineNumber">The source line of the <c>.init</c> or <c>.local</c> directive.</param>
	private sealed record LocalInitializer(int LocalIndex, string ValueToken, int LineNumber);

	/// <summary>Abstract base for all statements in a procedure body (either labels or instructions).</summary>
	/// <param name="LineNumber">The source line where this statement appears.</param>
	private abstract record Statement(int LineNumber);

	/// <summary>A code label declaration within a procedure body.</summary>
	/// <param name="Name">The label name (without the trailing colon).</param>
	/// <param name="LineNumber">The source line where this label appears.</param>
	private sealed record LabelStatement(string Name, int LineNumber) : Statement(LineNumber);

	/// <summary>An instruction with its mnemonic and operand tokens, as parsed from the source.</summary>
	/// <param name="Mnemonic">The instruction mnemonic as written in the source.</param>
	/// <param name="Operands">The operand tokens, in source order.</param>
	/// <param name="LineNumber">The source line where this instruction appears.</param>
	private sealed record InstructionDefinition(string Mnemonic, IReadOnlyList<string> Operands, int LineNumber) : Statement(LineNumber);

	/// <summary>Abstract base for all top-level RAM-section directives.</summary>
	/// <param name="LineNumber">The source line where this directive appears.</param>
	private abstract record RamDirective(int LineNumber);

	/// <summary>A <c>.ramorg</c> directive that repositions the current RAM word address.</summary>
	/// <param name="ValueToken">The raw word-address token.</param>
	/// <param name="LineNumber">The source line where this directive appears.</param>
	private sealed record RamOriginDirective(string ValueToken, int LineNumber) : RamDirective(LineNumber);

	/// <summary>
	/// An <c>.objstring</c> or <c>.objpacked</c> directive declaring a labeled item in the read-only
	/// object-data section that is appended to the OBJ file after the code.
	/// </summary>
	/// <param name="Label">The symbol label for this item.</param>
	/// <param name="StringValue">The string value for <c>.objstring</c>, or <see langword="null"/> for <c>.objpacked</c>.</param>
	/// <param name="PackedValues">The byte token list for <c>.objpacked</c>, or <see langword="null"/> for <c>.objstring</c>.</param>
	/// <param name="LineNumber">The source line where this directive appears.</param>
	private sealed record ObjDataDirective(string Label, string? StringValue, IReadOnlyList<string>? PackedValues, int LineNumber);

	/// <summary>The kind of value a data directive contains.</summary>
	private enum DataDirectiveKind
	{
		String,
		Words,
		Bytes,
	}

	/// <summary>A <c>.string</c>, <c>.word</c>/<c>.words</c>, or <c>.byte</c>/<c>.bytes</c> directive declaring labeled data in RAM.</summary>
	/// <param name="Kind">The kind of data this directive contains.</param>
	/// <param name="Label">The symbol label for this data block.</param>
	/// <param name="Values">The raw value tokens, used for word and byte directives.</param>
	/// <param name="StringValue">The unescaped string value, used for string directives.</param>
	/// <param name="LineNumber">The source line where this directive appears.</param>
	private sealed record DataDirective(DataDirectiveKind Kind, string Label, IReadOnlyList<string> Values, string? StringValue, int LineNumber) : RamDirective(LineNumber);

	/// <summary>A layout-allocated item in the object-data section with its computed byte offset.</summary>
	/// <param name="Label">The symbol label for this item.</param>
	/// <param name="ByteOffset">The byte offset of this item within the object-data section bytes.</param>
	/// <param name="Bytes">The encoded bytes for this item.</param>
	/// <param name="LineNumber">The source line where this item was declared.</param>
	private sealed record AllocatedObjDataItem(string Label, int ByteOffset, byte[] Bytes, int LineNumber);

	/// <summary>A layout-allocated RAM data block with its word address and data directives.</summary>
	/// <param name="Label">The symbol label for this RAM block.</param>
	/// <param name="WordAddress">The word address at which this block starts in RAM.</param>
	/// <param name="Directives">The data directives making up this block's content.</param>
	/// <param name="ByteLength">The byte length of this block's content, padded to an even number.</param>
	private sealed record AllocatedRamItem(string Label, ushort WordAddress, IReadOnlyList<DataDirective> Directives, int ByteLength);

	/// <summary>The result of laying out all RAM directives.</summary>
	/// <param name="Bytes">The initial RAM byte image.</param>
	/// <param name="InitialRamWords">The number of word-sized RAM slots populated by the initial image.</param>
	/// <param name="Symbols">The RAM data symbol table.</param>
	private sealed record RamLayout(byte[] Bytes, int InitialRamWords, IReadOnlyDictionary<string, DataSymbol> Symbols);

	/// <summary>The result of laying out the object-data section.</summary>
	/// <param name="Bytes">The encoded object-data bytes.</param>
	/// <param name="Symbols">The object-data symbol table.</param>
	private sealed record ObjDataLayout(byte[] Bytes, IReadOnlyDictionary<string, DataSymbol> Symbols);

	/// <summary>The complete assembled OBJ file and the length of its code-only portion.</summary>
	/// <param name="Bytes">The full OBJ file bytes, including both code and object-data sections.</param>
	/// <param name="CodeByteLength">The byte length of the code portion alone (before the object-data section).</param>
	private sealed record ObjectImageLayout(byte[] Bytes, int CodeByteLength);

	/// <summary>
	/// Records the layout of a single procedure within a module: its placement, statement offsets, and encoded size.
	/// </summary>
	/// <param name="Module">The module that contains this procedure.</param>
	/// <param name="Procedure">The procedure definition.</param>
	/// <param name="StartOffset">The module-relative byte offset at which the procedure header begins.</param>
	/// <param name="CodeOffset">The module-relative byte offset at which the procedure's code begins (after the header).</param>
	/// <param name="StatementOffsets">Maps each statement to its module-relative byte offset.</param>
	/// <param name="LocalLabels">Maps each local label name to its module-relative byte offset.</param>
	/// <param name="ByteLength">The total byte length of this procedure (header plus code).</param>
	private sealed record ProcedureLayout(
		ModuleDefinition Module,
		ProcedureDefinition Procedure,
		int StartOffset,
		int CodeOffset,
		IReadOnlyDictionary<Statement, int> StatementOffsets,
		IReadOnlyDictionary<string, int> LocalLabels,
		int ByteLength);

	/// <summary>A named RAM or object-data symbol resolved to a word address.</summary>
	/// <param name="Name">The symbol name.</param>
	/// <param name="WordAddress">The word address where this symbol's data lives.</param>
	/// <param name="LineNumber">The source line where this symbol was declared.</param>
	private sealed record DataSymbol(string Name, ushort WordAddress, int LineNumber);

	/// <summary>A named procedure symbol resolved to its module ID, procedure-table index, and module-relative offset.</summary>
	/// <param name="ModuleName">The name of the module that contains this procedure.</param>
	/// <param name="ProcedureName">The procedure name.</param>
	/// <param name="ModuleId">The 1-based module ID.</param>
	/// <param name="ProcedureIndex">The 0-based procedure-table index, or -1 for private procedures.</param>
	/// <param name="ModuleOffset">The module-relative byte offset at which the procedure header begins.</param>
	/// <param name="LineNumber">The source line where this procedure was declared.</param>
	private sealed record ProcedureSymbol(string ModuleName, string ProcedureName, int ModuleId, int ProcedureIndex, int ModuleOffset, int LineNumber);

	/// <summary>A named code label resolved to its module-relative byte offset.</summary>
	/// <param name="ModuleName">The name of the module containing the label.</param>
	/// <param name="ProcedureName">The name of the procedure containing the label.</param>
	/// <param name="LabelName">The label name.</param>
	/// <param name="ModuleOffset">The module-relative byte offset of the label.</param>
	/// <param name="LineNumber">The source line of the procedure that contains the label.</param>
	private sealed record CodeLabelSymbol(string ModuleName, string ProcedureName, string LabelName, int ModuleOffset, int LineNumber);

	/// <summary>
	/// The final output of a successful assembly: the OBJ and MME binary images and key summary metadata.
	/// </summary>
	/// <param name="ObjectBytes">The assembled OBJ file bytes.</param>
	/// <param name="MetadataBytes">The assembled MME metadata file bytes.</param>
	/// <param name="ModuleLayouts">The layout information for each module, useful for diagnostics.</param>
	/// <param name="InitialRamWords">The number of word-sized RAM slots in the initial RAM image.</param>
	/// <param name="EntryModuleId">The 1-based module ID of the entry procedure.</param>
	/// <param name="EntryProcedureIndex">The 0-based procedure-table index of the entry procedure.</param>
	private sealed record AssemblyImage(byte[] ObjectBytes, byte[] MetadataBytes, IReadOnlyList<ModuleLayout> ModuleLayouts, int InitialRamWords, int EntryModuleId, int EntryProcedureIndex);

	/// <summary>Represents an error encountered during source parsing or instruction assembly.</summary>
	private sealed class AssemblerException : Exception
	{
		public AssemblerException(string message)
			: base(message)
		{
		}
	}

	/// <summary>Returns the fully qualified dictionary key for a procedure (e.g. <c>Module.Procedure</c>).</summary>
	/// <param name="moduleName">The module name.</param>
	/// <param name="procedureName">The procedure name.</param>
	/// <returns>A dot-separated qualified key.</returns>
	private static string QualifyProcedure(string moduleName, string procedureName) => $"{moduleName}.{procedureName}";

	/// <summary>Returns the fully qualified dictionary key for a local code label (e.g. <c>Module.Procedure.Label</c>).</summary>
	/// <param name="moduleName">The module name.</param>
	/// <param name="procedureName">The procedure name.</param>
	/// <param name="label">The label name.</param>
	/// <returns>A dot-separated qualified key.</returns>
	private static string QualifyLabel(string moduleName, string procedureName, string label) => $"{moduleName}.{procedureName}.{label}";

	/// <summary>Returns the encoded byte size for a given grammar operand kind.</summary>
	/// <param name="kind">The operand kind string as it appears in the grammar JSON.</param>
	/// <returns>The number of bytes this operand occupies in the encoded instruction stream.</returns>
	private static int OperandSizeBytes(string kind) => kind switch
	{
		"immediate_u8" or "local_index_u8" or "aggregate_byte_index_u8" or "aggregate_word_index_u8" or "argument_count_u8"
			or "open_mode_u8" or "program_global_index_u8" or "module_global_index_u8" or "return_count_u8"
			or "stack_drop_count_u8" or "display_subop_u8" or "display_ext_subop_u8" => 1,
		"immediate_u16le" or "status_code_u16le" or "bitfield_control_u16le" or "near_code_offset_u16le"
			or "far_proc_selector_u16le" or "packed_code_location_u16le" => 2,
		"jump_target_mixed" => 3,
		_ => throw new AssemblerException($"Unsupported operand kind '{kind}'."),
	};

	/// <summary>
	/// Computes the total byte size of all operands that follow operand <paramref name="operandIndex"/> in
	/// the given opcode's operand list. Used when encoding a mixed-size branch operand to determine how many
	/// bytes come after it.
	/// </summary>
	/// <param name="opcode">The opcode whose trailing operands are being measured.</param>
	/// <param name="operandIndex">The index of the current operand; only operands after this index are counted.</param>
	/// <returns>The total byte count of all trailing operands.</returns>
	private static int GetFixedTrailingOperandSize(OpcodeInfo opcode, int operandIndex)
	{
		int total = 0;
		for (int index = operandIndex + 1; index < opcode.Operands.Count; index++)
		{
			GrammarOperand operand = opcode.Operands[index];
			if (operand.Kind == "jump_target_mixed")
			{
				throw new AssemblerException($"Internal error: opcode '{opcode.Mnemonic}' has multiple mixed jump operands.");
			}

			total += OperandSizeBytes(operand.Kind);
		}

		return total;
	}

	/// <summary>Returns the number of bytes remaining in the current 256-byte block.</summary>
	/// <param name="offset">The current module-relative byte offset.</param>
	/// <returns>The number of bytes from <paramref name="offset"/> to the end of the current 256-byte block.</returns>
	private static int RemainingBytesInBlock(int offset) => 0x100 - (offset & 0xFF);

	/// <summary>
	/// Returns the offset aligned to the start of the next 256-byte block if the procedure header
	/// does not fit in the remaining bytes of the current block; otherwise returns the offset unchanged.
	/// </summary>
	/// <param name="offset">The current module-relative byte offset.</param>
	/// <param name="headerSize">The byte size of the procedure header.</param>
	/// <param name="lineNumber">The source line number, used in error messages.</param>
	/// <returns>The aligned start offset for the procedure.</returns>
	private static int AlignProcedureStart(int offset, int headerSize, int lineNumber)
	{
		if (headerSize > 0x100)
		{
			throw new AssemblerException($"Line {lineNumber}: procedure header is 0x{headerSize:X} bytes, which cannot fit within a single 256-byte block.");
		}

		return headerSize > RemainingBytesInBlock(offset)
			? offset + RemainingBytesInBlock(offset)
			: offset;
	}

	/// <summary>
	/// Returns the number of bytes available at <paramref name="offset"/> before the assembler would
	/// automatically insert a NEXTB instruction. The last byte of each block is always reserved for NEXTB,
	/// so an instruction must fit in <c>N-1</c> remaining bytes to avoid triggering an auto-NEXTB.
	/// </summary>
	/// <param name="offset">The current module-relative byte offset.</param>
	/// <returns>The number of bytes available before auto-NEXTB would trigger, or 0 if at a block boundary.</returns>
	private static int AvailableBytesBeforeAutoNextBlock(int offset)
	{
		int remaining = RemainingBytesInBlock(offset);
		return remaining <= 1 ? 0 : remaining - 1;
	}

	/// <summary>Rounds a non-negative integer up to the nearest even number.</summary>
	/// <param name="value">The value to round up.</param>
	/// <returns>The smallest even number greater than or equal to <paramref name="value"/>.</returns>
	private static int AlignEven(int value) => (value + 1) & ~1;

	/// <summary>Parses a <c>0xNN</c> hex string token as a byte value.</summary>
	/// <param name="token">The token to parse (must start with <c>0x</c>).</param>
	/// <returns>The parsed byte value.</returns>
	private static byte ParseHexByte(string token)
	{
		return byte.Parse(token.AsSpan(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
	}

	/// <summary>Appends a 16-bit value in little-endian byte order to a byte list.</summary>
	/// <param name="bytes">The byte list to append to.</param>
	/// <param name="value">The 16-bit value to write.</param>
	private static void WriteWord(List<byte> bytes, ushort value)
	{
		bytes.Add((byte)(value & 0xFF));
		bytes.Add((byte)(value >> 8));
	}

	/// <summary>
	/// Evaluates a word-sized operand token using the provided symbol catalog,
	/// supporting arithmetic expressions, data symbols, and constants.
	/// </summary>
	/// <param name="token">The operand token or arithmetic expression.</param>
	/// <param name="lineNumber">The source line number, used in error messages.</param>
	/// <param name="symbols">The symbol catalog for resolving symbols and constants.</param>
	/// <returns>The resolved 16-bit value.</returns>
	private static ushort EvaluateWordOperand(string token, int lineNumber, SymbolCatalog symbols)
	{
		int value = EvaluateWordExpression(
			token,
			lineNumber,
			symbol => symbols.ResolveWordOperand(symbol, lineNumber),
			symbol => symbols.ResolveDataByteAddress(symbol, lineNumber));
		if (value < short.MinValue || value > ushort.MaxValue)
		{
			throw new AssemblerException($"Line {lineNumber}: value '{token}' does not fit in a word.");
		}

		return unchecked((ushort)value);
	}

	/// <summary>
	/// Evaluates a word-sized operand token using raw symbol dictionaries rather than a <see cref="SymbolCatalog"/>.
	/// Used during layout phases when only partial symbol information is available.
	/// </summary>
	/// <param name="token">The operand token or arithmetic expression.</param>
	/// <param name="lineNumber">The source line number, used in error messages.</param>
	/// <param name="dataSymbols">The data symbol dictionary for resolving symbol names.</param>
	/// <param name="constants">The constant definition dictionary, or <see langword="null"/> if constants are not available.</param>
	/// <returns>The resolved 16-bit value.</returns>
	private static ushort EvaluateWordOperand(string token, int lineNumber, IReadOnlyDictionary<string, DataSymbol> dataSymbols, IReadOnlyDictionary<string, ConstantDefinition>? constants = null)
	{
		int value = EvaluateWordExpression(
			token,
			lineNumber,
			symbol =>
			{
				if (constants is not null && constants.TryGetValue(symbol, out ConstantDefinition? constant))
				{
					return EvaluateWordOperand(constant.ValueToken, constant.LineNumber, dataSymbols, constants);
				}

				if (dataSymbols.TryGetValue(symbol, out DataSymbol? resolved))
				{
					return resolved.WordAddress;
				}

				throw new AssemblerException($"Line {lineNumber}: unknown symbol '{symbol}'.");
			},
			symbol =>
			{
				if (dataSymbols.TryGetValue(symbol, out DataSymbol? resolved))
				{
					return checked((ushort)(resolved.WordAddress * 2));
				}

				throw new AssemblerException($"Line {lineNumber}: unknown symbol '{symbol}'.");
			});
		if (value < short.MinValue || value > ushort.MaxValue)
		{
			throw new AssemblerException($"Line {lineNumber}: value '{token}' does not fit in a word.");
		}

		return unchecked((ushort)value);
	}

	/// <summary>
	/// Recursively evaluates an arithmetic word expression, supporting addition, subtraction,
	/// parenthesized sub-expressions, integer literals, <c>byte:</c>-prefixed symbol references,
	/// and arbitrary symbol names.
	/// </summary>
	/// <param name="token">The expression string to evaluate.</param>
	/// <param name="lineNumber">The source line number, used in error messages.</param>
	/// <param name="resolveWordSymbol">A callback to resolve a bare symbol name to a word value.</param>
	/// <param name="resolveByteSymbol">A callback to resolve a <c>byte:</c>-prefixed symbol name to a byte address.</param>
	/// <returns>The integer result of evaluating the expression.</returns>
	private static int EvaluateWordExpression(
		string token,
		int lineNumber,
		Func<string, ushort> resolveWordSymbol,
		Func<string, ushort> resolveByteSymbol)
	{
		token = token.Trim();

		while (HasBalancedOuterParentheses(token))
		{
			token = token[1..^1].Trim();
		}

		int additiveIndex = FindTopLevelAdditiveOperator(token);
		if (additiveIndex >= 0)
		{
			int left = EvaluateWordExpression(token[..additiveIndex], lineNumber, resolveWordSymbol, resolveByteSymbol);
			int right = EvaluateWordExpression(token[(additiveIndex + 1)..], lineNumber, resolveWordSymbol, resolveByteSymbol);
			return token[additiveIndex] == '+' ? checked(left + right) : checked(left - right);
		}

		if (TryParseInteger(token, out int numeric))
		{
			return numeric;
		}

		const string bytePrefix = "byte:";
		if (token.StartsWith(bytePrefix, StringComparison.OrdinalIgnoreCase))
		{
			string symbol = token[bytePrefix.Length..].Trim();
			if (string.IsNullOrEmpty(symbol))
			{
				throw new AssemblerException($"Line {lineNumber}: malformed byte address expression '{token}'.");
			}

			return resolveByteSymbol(symbol);
		}

		return resolveWordSymbol(token);
	}

	/// <summary>Returns <see langword="true"/> if the token starts and ends with matching parentheses that span the entire string.</summary>
	/// <param name="token">The token to test.</param>
	/// <returns><see langword="true"/> if the outer parentheses are balanced and enclose the entire token.</returns>
	private static bool HasBalancedOuterParentheses(string token)
	{
		if (token.Length < 2 || token[0] != '(' || token[^1] != ')')
		{
			return false;
		}

		int depth = 0;
		for (int index = 0; index < token.Length; index++)
		{
			char ch = token[index];
			if (ch == '(')
			{
				depth++;
			}
			else if (ch == ')')
			{
				depth--;
				if (depth == 0 && index != token.Length - 1)
				{
					return false;
				}
			}
		}

		return depth == 0;
	}

	/// <summary>
	/// Scans from right to left for the rightmost <c>+</c> or <c>-</c> operator at top-level
	/// (not inside parentheses) with whitespace on at least one side, returning its index.
	/// Returns -1 if no such operator exists.
	/// </summary>
	/// <param name="token">The expression token to scan.</param>
	/// <returns>The index of the operator character, or -1 if not found.</returns>
	private static int FindTopLevelAdditiveOperator(string token)
	{
		int depth = 0;
		for (int index = token.Length - 1; index >= 0; index--)
		{
			char ch = token[index];
			if (ch == ')')
			{
				depth++;
			}
			else if (ch == '(')
			{
				depth--;
			}
			else if (depth == 0 && (ch == '+' || ch == '-'))
			{
				if (index == 0)
				{
					continue;
				}

				char previous = token[index - 1];
				if (previous == '+' || previous == '-' || previous == '(')
				{
					continue;
				}

				if (index == token.Length - 1)
				{
					continue;
				}

				char next = token[index + 1];
				if (!char.IsWhiteSpace(previous) && !char.IsWhiteSpace(next))
				{
					continue;
				}

				return index;
			}
		}

		return -1;
	}

	/// <summary>
	/// Evaluates a byte-sized operand token, accepting numeric literals, named constants, and
	/// optionally data symbol addresses.
	/// </summary>
	/// <param name="token">The operand token to evaluate.</param>
	/// <param name="lineNumber">The source line number, used in error messages.</param>
	/// <param name="symbols">The symbol catalog for resolving constants.</param>
	/// <param name="allowDataSymbols">If <see langword="true"/>, data symbol word addresses are also accepted as byte values.</param>
	/// <returns>The resolved byte value.</returns>
	private static byte EvaluateByteOperand(string token, int lineNumber, SymbolCatalog symbols, bool allowDataSymbols = false)
	{
		if (TryParseInteger(token, out int numeric))
		{
			if (numeric < 0 || numeric > 0xFF)
			{
				throw new AssemblerException($"Line {lineNumber}: value '{token}' does not fit in a byte.");
			}

			return (byte)numeric;
		}

		if (symbols.TryResolveConstantNumeric(token, lineNumber, out int constantValue))
		{
			if (constantValue < 0 || constantValue > 0xFF)
			{
				throw new AssemblerException($"Line {lineNumber}: value '{token}' does not fit in a byte.");
			}

			return (byte)constantValue;
		}

		if (allowDataSymbols && symbols.DataSymbols.TryGetValue(token, out DataSymbol? symbol))
		{
			if (symbol.WordAddress > 0xFF)
			{
				throw new AssemblerException($"Line {lineNumber}: symbol '{token}' does not fit in a byte.");
			}

			return (byte)symbol.WordAddress;
		}

		throw new AssemblerException($"Line {lineNumber}: unknown byte operand '{token}'.");
	}

	/// <summary>Parses a numeric literal token and validates that it falls within the specified inclusive range.</summary>
	/// <param name="token">The token to parse.</param>
	/// <param name="lineNumber">The source line number, used in error messages.</param>
	/// <param name="minInclusive">The minimum acceptable value.</param>
	/// <param name="maxInclusive">The maximum acceptable value.</param>
	/// <returns>The parsed integer value.</returns>
	private static int EvaluateNumericLiteral(string token, int lineNumber, int minInclusive, int maxInclusive)
	{
		if (!TryParseInteger(token, out int numeric))
		{
			throw new AssemblerException($"Line {lineNumber}: expected a numeric literal, found '{token}'.");
		}

		if (numeric < minInclusive || numeric > maxInclusive)
		{
			throw new AssemblerException($"Line {lineNumber}: value '{token}' is outside the supported range {minInclusive}..{maxInclusive}.");
		}

		return numeric;
	}

	/// <summary>Tries to parse a token as a decimal integer, hexadecimal integer, or single-quoted character literal.</summary>
	/// <param name="token">The token to parse.</param>
	/// <param name="value">When this method returns <see langword="true"/>, the parsed integer value.</param>
	/// <returns><see langword="true"/> if the token is a recognized numeric literal; otherwise <see langword="false"/>.</returns>
	private static bool TryParseInteger(string token, out int value)
	{
		if (IsQuotedChar(token))
		{
			string unescaped = UnescapeChar(token);
			value = unescaped[0];
			return true;
		}

		if (token.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
		{
			return int.TryParse(token.AsSpan(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out value);
		}

		if (token.StartsWith("-0x", StringComparison.OrdinalIgnoreCase))
		{
			bool parsed = int.TryParse(token.AsSpan(3), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out int positive);
			value = -positive;
			return parsed;
		}

		return int.TryParse(token, NumberStyles.Integer, CultureInfo.InvariantCulture, out value);
	}

	/// <summary>Returns <see langword="true"/> if the token is a double-quoted string literal.</summary>
	/// <param name="token">The token to test.</param>
	/// <returns><see langword="true"/> if the token starts and ends with a double-quote character.</returns>
	private static bool IsQuotedString(string token) => token.Length >= 2 && token[0] == '"' && token[^1] == '"';

	/// <summary>Returns <see langword="true"/> if the token is a single-quoted character literal.</summary>
	/// <param name="token">The token to test.</param>
	/// <returns><see langword="true"/> if the token is surrounded by single-quote characters.</returns>
	private static bool IsQuotedChar(string token) => token.Length >= 3 && token[0] == '\'' && token[^1] == '\'';

	/// <summary>Strips the enclosing double-quotes from a string literal and processes backslash escape sequences.</summary>
	/// <param name="token">The quoted string token (e.g. <c>"hello\nworld"</c>).</param>
	/// <returns>The unescaped string value.</returns>
	private static string UnescapeString(string token)
	{
		return UnescapeQuoted(token[1..^1]);
	}

	/// <summary>
	/// Strips the enclosing single-quotes from a character literal, processes backslash escape sequences,
	/// and validates that exactly one character results.
	/// </summary>
	/// <param name="token">The quoted character token (e.g. <c>'A'</c> or <c>'\n'</c>).</param>
	/// <returns>The single-character unescaped string.</returns>
	private static string UnescapeChar(string token)
	{
		string unescaped = UnescapeQuoted(token[1..^1]);
		if (unescaped.Length != 1)
		{
			throw new AssemblerException($"Character literal {token} must contain exactly one character.");
		}

		return unescaped;
	}

	/// <summary>Processes backslash escape sequences in the inner content of a quoted literal string or character.</summary>
	/// <param name="content">The content between the outer quote characters, with no surrounding quotes.</param>
	/// <returns>The string with all escape sequences replaced by their intended characters.</returns>
	private static string UnescapeQuoted(string content)
	{
		StringBuilder builder = new();
		for (int index = 0; index < content.Length; index++)
		{
			char character = content[index];
			if (character != '\\')
			{
				builder.Append(character);
				continue;
			}

			if (index + 1 >= content.Length)
			{
				throw new AssemblerException("Malformed escape sequence.");
			}

			char escaped = content[++index];
			builder.Append(escaped switch
			{
				'\\' => '\\',
				'"' => '"',
				'\'' => '\'',
				'n' => '\n',
				'r' => '\r',
				't' => '\t',
				_ => throw new AssemblerException($"Unsupported escape sequence '\\{escaped}'."),
			});
		}

		return builder.ToString();
	}
}
