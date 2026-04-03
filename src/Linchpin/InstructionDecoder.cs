/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Globalization;
using System.IO.Compression;
using System.Text.Json;

namespace Linchpin;

/// <summary>
/// Decodes Cornerstone VM bytecode instructions from raw OBJ bytes, using the opcode grammar
/// to map byte sequences to <see cref="DecodedInstruction"/> records.
/// </summary>
internal static class BytecodeDecoder
{
	/// <summary>
	/// Decodes a single instruction at the specified module-relative offset.
	/// </summary>
	/// <param name="image">The loaded Cornerstone image.</param>
	/// <param name="grammar">The opcode grammar defining instruction encoding.</param>
	/// <param name="module">The module containing the instruction.</param>
	/// <param name="moduleRelativeOffset">Module-relative byte offset of the instruction.</param>
	/// <param name="upperBound">Module-relative byte offset upper bound for the current procedure.</param>
	/// <returns>The decoded instruction.</returns>
	public static DecodedInstruction DecodeInstructionAt(CornerstoneImage image, InstructionGrammar grammar, ModuleImage module, int moduleRelativeOffset, int upperBound)
	{
		return DecodeInstruction(image.ObjectBytes, grammar, module, moduleRelativeOffset, upperBound);
	}

	/// <summary>
	/// Decodes a limited preview of instructions from a procedure, for display purposes.
	/// </summary>
	/// <param name="image">The loaded Cornerstone image.</param>
	/// <param name="grammar">The opcode grammar.</param>
	/// <param name="module">The module containing the procedure.</param>
	/// <param name="procedure">The procedure to decode.</param>
	/// <param name="maxInstructions">Maximum number of instructions to decode.</param>
	/// <returns>The decoded instructions.</returns>
	public static IReadOnlyList<DecodedInstruction> DecodeProcedurePreview(CornerstoneImage image, InstructionGrammar grammar, ModuleImage module, ProcedureEntry procedure, int maxInstructions)
	{
		int upperBound = GetProcedureUpperBound(module, procedure);

		List<DecodedInstruction> instructions = new();
		int offset = procedure.CodeOffset;
		while (offset < upperBound && instructions.Count < maxInstructions)
		{
			DecodedInstruction instruction = DecodeInstruction(image.ObjectBytes, grammar, module, offset, upperBound);
			instructions.Add(instruction);
			offset += instruction.ByteLength;
		}

		return instructions;
	}

	/// <summary>
	/// Decodes all instructions in a procedure up to the specified upper bound.
	/// </summary>
	/// <param name="image">The loaded Cornerstone image.</param>
	/// <param name="grammar">The opcode grammar.</param>
	/// <param name="module">The module containing the procedure.</param>
	/// <param name="procedure">The procedure to decode.</param>
	/// <param name="upperBound">Module-relative byte offset upper bound.</param>
	/// <returns>All decoded instructions in the procedure.</returns>
	public static IReadOnlyList<DecodedInstruction> DecodeProcedure(CornerstoneImage image, InstructionGrammar grammar, ModuleImage module, ProcedureEntry procedure, int upperBound)
	{
		List<DecodedInstruction> instructions = new();
		int offset = procedure.CodeOffset;

		while (offset < upperBound)
		{
			DecodedInstruction instruction = DecodeInstruction(image.ObjectBytes, grammar, module, offset, upperBound);
			instructions.Add(instruction);
			offset += instruction.ByteLength;
		}

		return instructions;
	}

	/// <summary>
	/// Returns the upper bound (exclusive) of a procedure's byte range within its module,
	/// defined as the start offset of the next procedure or the module end.
	/// </summary>
	public static int GetProcedureUpperBound(ModuleImage module, ProcedureEntry procedure)
	{
		return module.Procedures
			.Where(candidate => candidate.StartOffset > procedure.StartOffset)
			.Select(candidate => candidate.StartOffset)
			.DefaultIfEmpty(module.Length)
			.Min();
	}

	private static DecodedInstruction DecodeInstruction(byte[] objectBytes, InstructionGrammar grammar, ModuleImage module, int moduleRelativeOffset, int upperBound)
	{
		int absoluteOffset = module.ObjectOffset + moduleRelativeOffset;
		if (absoluteOffset >= objectBytes.Length)
		{
			throw new LinchpinException($"Decoder ran past the end of module {module.ModuleId} while reading 0x{moduleRelativeOffset:X4}.");
		}

		byte opcode = objectBytes[absoluteOffset];
		if (opcode >= 0x60)
		{
			return DecodePackedInstruction(opcode, moduleRelativeOffset);
		}

		if (opcode == 0x5F)
		{
			if (absoluteOffset + 1 >= objectBytes.Length)
			{
				throw new LinchpinException($"Decoder hit a truncated extended opcode in module {module.ModuleId} at 0x{moduleRelativeOffset:X4}.");
			}

			byte extendedOpcode = objectBytes[absoluteOffset + 1];
			OpcodeDefinition definition = grammar.GetExtended(extendedOpcode);
			return DecodeStructuredInstruction(objectBytes, module.ObjectOffset, moduleRelativeOffset, upperBound, definition, 2);
		}

		OpcodeDefinition primaryDefinition = grammar.GetPrimary(opcode);
		return DecodeStructuredInstruction(objectBytes, module.ObjectOffset, moduleRelativeOffset, upperBound, primaryDefinition, 1);
	}

	/// <summary>
	/// Decodes a packed single-byte instruction. Opcodes 0x60–0xFF encode common operations
	/// (VLOADW, PUSHL, PUTL, STOREL) with operands embedded in the opcode byte itself.
	/// </summary>
	private static DecodedInstruction DecodePackedInstruction(byte opcode, int offset)
	{
		if (opcode <= 0x9F)
		{
			int localIndex = opcode & 0x0F;
			int wordIndex = (opcode >> 4) & 0x03;
			return new DecodedInstruction(
				offset,
				1,
				"VLOADW",
				new[]
				{
					CreateNumericOperand("aggregate_word_index_u8", wordIndex),
					CreateNumericOperand("local_index_u8", localIndex)
				});
		}

		if (opcode <= 0xBF)
		{
			int localIndex = opcode & 0x1F;
			return new DecodedInstruction(offset, 1, "PUSHL", new[] { CreateNumericOperand("local_index_u8", localIndex) });
		}

		if (opcode <= 0xDF)
		{
			int localIndex = opcode & 0x1F;
			return new DecodedInstruction(offset, 1, "PUTL", new[] { CreateNumericOperand("local_index_u8", localIndex) });
		}

		int storeLocalIndex = opcode & 0x1F;
		return new DecodedInstruction(offset, 1, "STOREL", new[] { CreateNumericOperand("local_index_u8", storeLocalIndex) });
	}

	private static DecodedInstruction DecodeStructuredInstruction(byte[] objectBytes, int moduleObjectOffset, int offset, int upperBound, OpcodeDefinition definition, int prefixLength)
	{
		int absoluteOffset = moduleObjectOffset + offset;
		int cursor = absoluteOffset + prefixLength;
		List<DecodedOperand> operands = new();
		int totalLength = prefixLength;

		if (definition.Mnemonic.Equals("NEXTB", StringComparison.OrdinalIgnoreCase))
		{
			totalLength = RemainingBytesInBlock(offset);
			return new DecodedInstruction(offset, totalLength, definition.Mnemonic, operands);
		}

		foreach (GrammarOperand operand in definition.Operands)
		{
			DecodedOperand decodedOperand = DecodeOperand(objectBytes, offset, ref cursor, upperBound, operand);
			operands.Add(decodedOperand);
		}

		totalLength = cursor - absoluteOffset;
		return NormalizeDecodedInstruction(new DecodedInstruction(offset, totalLength, definition.Mnemonic, operands));
	}

	/// <summary>
	/// Normalizes synonymous mnemonics (e.g. PUSH0–PUSH8, PUSHB/PUSHW, LOADL) into canonical
	/// forms to simplify downstream consumers.
	/// </summary>
	private static DecodedInstruction NormalizeDecodedInstruction(DecodedInstruction instruction)
	{
		return instruction.Mnemonic switch
		{
			"PUSH0" => CreatePushInstruction(instruction, 0x0000),
			"PUSH1" => CreatePushInstruction(instruction, 0x0001),
			"PUSH2" => CreatePushInstruction(instruction, 0x0002),
			"PUSH3" => CreatePushInstruction(instruction, 0x0003),
			"PUSH4" => CreatePushInstruction(instruction, 0x0004),
			"PUSH5" => CreatePushInstruction(instruction, 0x0005),
			"PUSH6" => CreatePushInstruction(instruction, 0x0006),
			"PUSH7" => CreatePushInstruction(instruction, 0x0007),
			"PUSH8" => CreatePushInstruction(instruction, 0x0008),
			"PUSHFF" => CreatePushInstruction(instruction, 0x00FF),
			"PUSHm8" => CreatePushInstruction(instruction, 0xFFF8),
			"PUSHm1" => CreatePushInstruction(instruction, 0xFFFF),
			"PUSHB" or "PUSHW" => new DecodedInstruction(
				instruction.Offset,
				instruction.ByteLength,
				"PUSH",
				new[] { CreatePushOperand(instruction.Operands[0].RawValue) }),
			"VLOADW_" => new DecodedInstruction(
				instruction.Offset,
				instruction.ByteLength,
				"VLOADW",
				new[] { CreateNumericOperand("aggregate_word_index_u8", instruction.Operands[0].RawValue) }),
			"VLOADB_" => new DecodedInstruction(
				instruction.Offset,
				instruction.ByteLength,
				"VLOADB",
				new[] { CreateNumericOperand("aggregate_byte_index_u8", instruction.Operands[0].RawValue) }),
			"VPUTW_" => new DecodedInstruction(
				instruction.Offset,
				instruction.ByteLength,
				"VPUTW",
				new[] { CreateNumericOperand("aggregate_word_index_u8", instruction.Operands[0].RawValue) }),
			"LOADL" => new DecodedInstruction(
				instruction.Offset,
				instruction.ByteLength,
				"PUSHL",
				new[] { CreateNumericOperand("local_index_u8", instruction.Operands[0].RawValue) }),
			"PUSHL" or "PUTL" or "STOREL" => new DecodedInstruction(
				instruction.Offset,
				instruction.ByteLength,
				instruction.Mnemonic,
				new[] { CreateNumericOperand("local_index_u8", instruction.Operands[0].RawValue) }),
			"POPVB2" => new DecodedInstruction(instruction.Offset, instruction.ByteLength, "PUTVB2", instruction.Operands),
			_ => instruction,
		};
	}

	private static DecodedInstruction CreatePushInstruction(DecodedInstruction instruction, int value)
	{
		return new DecodedInstruction(
			instruction.Offset,
			instruction.ByteLength,
			"PUSH",
			new[] { CreatePushOperand(value) });
	}

	private static DecodedOperand CreateNumericOperand(string kind, int value)
	{
		return new DecodedOperand(kind, value, value.ToString(CultureInfo.InvariantCulture));
	}

	private static DecodedOperand CreatePushOperand(int value)
	{
		return new DecodedOperand("immediate_u16le", value, FormatPushLiteral(value));
	}

	private static string FormatPushLiteral(int value)
	{
		ushort word = unchecked((ushort)value);
		return word switch
		{
			0xFFFF => "-1",
			0xFFF8 => "-8",
			_ when word <= 0x00FF => word.ToString(CultureInfo.InvariantCulture),
			_ => $"0x{word:X4}",
		};
	}

	private static DecodedOperand DecodeOperand(byte[] objectBytes, int instructionOffset, ref int cursor, int upperBound, GrammarOperand operand)
	{
		EnsureReadable(objectBytes, cursor, OperandSizeHint(operand.Kind), instructionOffset, operand.Kind);

		return operand.Kind switch
		{
			"immediate_u8" => DecodeByteOperand(objectBytes, ref cursor, operand.Kind, value => $"0x{value:X2}"),
			"local_index_u8" => DecodeByteOperand(objectBytes, ref cursor, operand.Kind, value => $"L{value}"),
			"aggregate_byte_index_u8" => DecodeByteOperand(objectBytes, ref cursor, operand.Kind, value => $"byte[{value}]"),
			"aggregate_word_index_u8" => DecodeByteOperand(objectBytes, ref cursor, operand.Kind, value => $"word[{value}]"),
			"argument_count_u8" => DecodeByteOperand(objectBytes, ref cursor, operand.Kind, value => value.ToString()),
			"open_mode_u8" => DecodeByteOperand(objectBytes, ref cursor, operand.Kind, value => $"0x{value:X2}"),
			"program_global_index_u8" => DecodeByteOperand(objectBytes, ref cursor, operand.Kind, value => $"PG[0x{value:X2}]"),
			"module_global_index_u8" => DecodeByteOperand(objectBytes, ref cursor, operand.Kind, value => $"MG[0x{value:X2}]"),
			"return_count_u8" => DecodeByteOperand(objectBytes, ref cursor, operand.Kind, value => value.ToString()),
			"stack_drop_count_u8" => DecodeByteOperand(objectBytes, ref cursor, operand.Kind, value => value.ToString()),
			"display_subop_u8" => DecodeByteOperand(objectBytes, ref cursor, operand.Kind, value => $"0x{value:X2}"),
			"display_ext_subop_u8" => DecodeByteOperand(objectBytes, ref cursor, operand.Kind, value => $"0x{value:X2}"),
			"immediate_u16le" => DecodeWordOperand(objectBytes, ref cursor, operand.Kind, value => $"0x{value:X4}"),
			"status_code_u16le" => DecodeWordOperand(objectBytes, ref cursor, operand.Kind, value => $"0x{value:X4}"),
			"bitfield_control_u16le" => DecodeWordOperand(objectBytes, ref cursor, operand.Kind, value => $"0x{value:X4}"),
			"near_code_offset_u16le" => DecodeWordOperand(objectBytes, ref cursor, operand.Kind, value => $"0x{value:X4}"),
			"far_proc_selector_u16le" => DecodeWordOperand(objectBytes, ref cursor, operand.Kind, value => $"module {value >> 8}, proc {value & 0xFF}"),
			"packed_code_location_u16le" => DecodeWordOperand(objectBytes, ref cursor, operand.Kind, value => $"0x{value:X4}"),
			"jump_target_mixed" => DecodeJumpOperand(objectBytes, instructionOffset, ref cursor, upperBound),
			_ => throw new LinchpinException($"Unsupported operand kind '{operand.Kind}' in decoder."),
		};
	}

	private static DecodedOperand DecodeByteOperand(byte[] objectBytes, ref int cursor, string kind, Func<int, string> formatter)
	{
		int value = objectBytes[cursor++];
		return new DecodedOperand(kind, value, formatter(value));
	}

	private static DecodedOperand DecodeWordOperand(byte[] objectBytes, ref int cursor, string kind, Func<int, string> formatter)
	{
		int value = objectBytes[cursor] | (objectBytes[cursor + 1] << 8);
		cursor += 2;
		return new DecodedOperand(kind, value, formatter(value));
	}

	/// <summary>
	/// Decodes a jump-target operand. A nonzero first byte is a signed relative displacement;
	/// a zero first byte is followed by a two-byte absolute target address.
	/// </summary>
	private static DecodedOperand DecodeJumpOperand(byte[] objectBytes, int instructionOffset, ref int cursor, int upperBound)
	{
		byte selector = objectBytes[cursor++];
		if (selector != 0)
		{
			int displacement = unchecked((sbyte)selector);
			int target = instructionOffset + 2 + displacement;
			return new DecodedOperand("jump_target_mixed", target, $"0x{target:X4} ({displacement:+#;-#;0})");
		}

		EnsureReadable(objectBytes, cursor, 2, instructionOffset, "jump_target_mixed absolute target");
		int absoluteTarget = objectBytes[cursor] | (objectBytes[cursor + 1] << 8);
		cursor += 2;
		if (absoluteTarget >= upperBound)
		{
			return new DecodedOperand("jump_target_mixed", absoluteTarget, $"0x{absoluteTarget:X4} (abs, beyond current bound)");
		}

		return new DecodedOperand("jump_target_mixed", absoluteTarget, $"0x{absoluteTarget:X4} (abs)");
	}

	private static void EnsureReadable(byte[] objectBytes, int cursor, int size, int instructionOffset, string description)
	{
		if (cursor + size - 1 >= objectBytes.Length)
		{
			throw new LinchpinException($"Decoder hit truncated bytes while reading {description} at 0x{instructionOffset:X4}.");
		}
	}

	private static int OperandSizeHint(string kind) => kind switch
	{
		"immediate_u8" or "local_index_u8" or "aggregate_byte_index_u8" or "aggregate_word_index_u8" or "argument_count_u8"
			or "open_mode_u8" or "program_global_index_u8" or "module_global_index_u8" or "return_count_u8"
			or "stack_drop_count_u8" or "display_subop_u8" or "display_ext_subop_u8" => 1,
		"immediate_u16le" or "status_code_u16le" or "bitfield_control_u16le" or "near_code_offset_u16le"
			or "far_proc_selector_u16le" or "packed_code_location_u16le" => 2,
		"jump_target_mixed" => 1,
		_ => 0,
	};

	/// <summary>
	/// Returns the number of remaining bytes in the current 256-byte block. Used to
	/// calculate the NEXTB instruction's implicit padding length.
	/// </summary>
	private static int RemainingBytesInBlock(int offset) => 0x100 - (offset & 0xFF);
}

/// <summary>
/// Holds the primary and extended opcode tables loaded from the instruction grammar JSON.
/// Maps opcode bytes to <see cref="OpcodeDefinition"/> records.
/// </summary>
internal sealed class InstructionGrammar
{
	private readonly Dictionary<byte, OpcodeDefinition> primaryOpcodes;
	private readonly Dictionary<byte, OpcodeDefinition> extendedOpcodes;

	private InstructionGrammar(Dictionary<byte, OpcodeDefinition> primaryOpcodes, Dictionary<byte, OpcodeDefinition> extendedOpcodes)
	{
		this.primaryOpcodes = primaryOpcodes;
		this.extendedOpcodes = extendedOpcodes;
	}

	/// <summary>
	/// Loads an instruction grammar from a JSON file, or from the embedded resource if no path is given.
	/// </summary>
	/// <param name="path">Path to a grammar JSON file, or <c>null</c> to use the embedded default grammar.</param>
	/// <returns>The loaded grammar.</returns>
	public static InstructionGrammar Load(string? path)
	{
		using Stream stream = OpenGrammarStream(path);
		using JsonDocument document = JsonDocument.Parse(stream);
		Dictionary<byte, OpcodeDefinition> primaryOpcodes = new();
		Dictionary<byte, OpcodeDefinition> extendedOpcodes = new();

		foreach (JsonElement entry in document.RootElement.GetProperty("primary_opcode_table").EnumerateArray())
		{
			string form = entry.GetProperty("form").GetString() ?? string.Empty;
			if (form == "extension_prefix")
			{
				continue;
			}

			byte opcode = ParseHexByte(entry.GetProperty("opcode").GetString());
			primaryOpcodes[opcode] = new OpcodeDefinition(
				entry.GetProperty("mnemonic").GetString() ?? throw new LinchpinException("Malformed primary opcode entry."),
				ParseOperands(entry.GetProperty("operands")));
		}

		foreach (JsonElement entry in document.RootElement.GetProperty("extended_opcode_table").EnumerateArray())
		{
			byte opcode = ParseHexByte(entry.GetProperty("opcode").GetString());
			extendedOpcodes[opcode] = new OpcodeDefinition(
				entry.GetProperty("mnemonic").GetString() ?? throw new LinchpinException("Malformed extended opcode entry."),
				ParseOperands(entry.GetProperty("operands")));
		}

		return new InstructionGrammar(primaryOpcodes, extendedOpcodes);
	}

	private static Stream OpenGrammarStream(string? path)
	{
		if (path is not null)
		{
			if (!File.Exists(path))
			{
				throw new LinchpinException($"Grammar file '{path}' does not exist.");
			}

			return File.OpenRead(path);
		}

		Stream? resource = typeof(InstructionGrammar).Assembly
			.GetManifestResourceStream("Linchpin.cornerstone_instruction_grammar.json.gz");
		if (resource is null)
		{
			throw new LinchpinException("Embedded grammar resource not found.");
		}

		return new GZipStream(resource, CompressionMode.Decompress);
	}

	/// <summary>
	/// Looks up a primary opcode definition (opcodes 0x00–0x5E).
	/// </summary>
	/// <param name="opcode">The primary opcode byte.</param>
	/// <returns>The opcode definition.</returns>
	public OpcodeDefinition GetPrimary(byte opcode)
	{
		if (!primaryOpcodes.TryGetValue(opcode, out OpcodeDefinition? definition))
		{
			throw new LinchpinException($"No primary opcode definition exists for 0x{opcode:X2}.");
		}

		return definition;
	}

	/// <summary>
	/// Looks up an extended opcode definition (opcodes prefixed by 0x5F).
	/// </summary>
	/// <param name="opcode">The extended opcode byte (after the 0x5F prefix).</param>
	/// <returns>The opcode definition.</returns>
	public OpcodeDefinition GetExtended(byte opcode)
	{
		if (!extendedOpcodes.TryGetValue(opcode, out OpcodeDefinition? definition))
		{
			throw new LinchpinException($"No extended opcode definition exists for 0x5F 0x{opcode:X2}.");
		}

		return definition;
	}

	private static List<GrammarOperand> ParseOperands(JsonElement element)
	{
		List<GrammarOperand> operands = new();
		foreach (JsonElement operand in element.EnumerateArray())
		{
			operands.Add(new GrammarOperand(
				operand.GetProperty("name").GetString() ?? string.Empty,
				operand.GetProperty("kind").GetString() ?? string.Empty));
		}

		return operands;
	}

	private static byte ParseHexByte(string? value)
	{
		if (string.IsNullOrWhiteSpace(value) || !value.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
		{
			throw new LinchpinException("Malformed opcode value in grammar file.");
		}

		return Convert.ToByte(value, 16);
	}
}

/// <summary>
/// Describes a single opcode: its mnemonic and the operand encoding it expects.
/// </summary>
internal sealed record OpcodeDefinition(string Mnemonic, IReadOnlyList<GrammarOperand> Operands);

/// <summary>
/// Describes a named operand slot in an opcode's encoding.
/// </summary>
internal sealed record GrammarOperand(string Name, string Kind);

/// <summary>
/// A decoded VM instruction at a specific module-relative offset, with its mnemonic and operands.
/// </summary>
internal sealed record DecodedInstruction(int Offset, int ByteLength, string Mnemonic, IReadOnlyList<DecodedOperand> Operands);

/// <summary>
/// A single decoded operand value from an instruction, including its kind, raw value, and display text.
/// </summary>
internal sealed record DecodedOperand(string Kind, int RawValue, string DisplayText);