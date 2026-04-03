/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Linchpin;

/// <summary>
/// Performs whole-image disassembly analysis: iterates through modules, discovers private procedures
/// via near-call tracing, computes call graphs, and identifies inter-procedure gaps.
/// </summary>
internal static class DisassemblyAnalyzer
{
	/// <summary>
	/// Analyzes every module in the image, decoding all procedures, discovering private call targets,
	/// computing outgoing and incoming call references, and identifying gaps between procedures.
	/// </summary>
	/// <param name="image">The loaded Cornerstone image.</param>
	/// <param name="grammar">The opcode grammar for instruction decoding.</param>
	/// <returns>A complete disassembly of the image.</returns>
	public static ImageDisassembly Analyze(CornerstoneImage image, InstructionGrammar grammar)
	{
		Dictionary<ProcedureKey, ProcedureDisassembly> procedures = new();
		Dictionary<int, IReadOnlyList<ModuleGap>> moduleGaps = new();
		Dictionary<(int ModuleId, int ExportedProcedureIndex), ProcedureKey> exportedProcedures = new();

		foreach (ModuleImage module in image.Modules)
		{
			IReadOnlyList<ProcedureDisassembly> moduleProcedures = AnalyzeModule(image, grammar, module);
			foreach (ProcedureDisassembly disassembly in moduleProcedures)
			{
				procedures.Add(disassembly.Key, disassembly);
				if (disassembly.Procedure.ExportedProcedureIndex is int exportedProcedureIndex)
				{
					exportedProcedures.Add((module.ModuleId, exportedProcedureIndex), disassembly.Key);
				}
			}

			moduleGaps.Add(module.ModuleId, AnalyzeModuleGaps(image.ObjectBytes, module, moduleProcedures));
		}

		Dictionary<ProcedureKey, List<CallReference>> incomingCalls = procedures.Keys.ToDictionary(key => key, _ => new List<CallReference>());
		foreach (ProcedureDisassembly procedure in procedures.Values)
		{
			foreach (CallReference call in procedure.OutgoingCalls)
			{
				if (call.Target is ProcedureKey target && incomingCalls.TryGetValue(target, out List<CallReference>? callers))
				{
					callers.Add(call);
				}
			}
		}

		Dictionary<ProcedureKey, ProcedureDisassembly> finalized = new();
		foreach ((ProcedureKey key, ProcedureDisassembly procedure) in procedures)
		{
			finalized.Add(key, procedure with { IncomingCalls = incomingCalls[key].OrderBy(call => call.Source.ModuleId).ThenBy(call => call.Source.StartOffset).ThenBy(call => call.InstructionOffset).ToArray() });
		}

		return new ImageDisassembly(finalized, moduleGaps, exportedProcedures);
	}

	/// <summary>
	/// Iteratively analyzes a module's procedures, re-decoding after each round of discovery
	/// until no new near-call targets are found.
	/// </summary>
	private static IReadOnlyList<ProcedureDisassembly> AnalyzeModule(CornerstoneImage image, InstructionGrammar grammar, ModuleImage module)
	{
		Dictionary<int, ProcedureEntry> knownProcedures = module.Procedures.ToDictionary(procedure => procedure.StartOffset);
		while (true)
		{
			List<ProcedureEntry> orderedProcedures = knownProcedures.Values.OrderBy(procedure => procedure.StartOffset).ToList();
			Dictionary<int, ProcedureDisassembly> analyzed = orderedProcedures.ToDictionary(
				procedure => procedure.StartOffset,
				procedure => AnalyzeProcedure(image, grammar, module, procedure, orderedProcedures));

			bool discoveredNewProcedure = false;
			foreach (ProcedureDisassembly disassembly in analyzed.Values)
			{
				foreach (int nearTargetOffset in disassembly.OutgoingCalls.Where(call => !call.IsFar && call.Target is ProcedureKey).Select(call => call.Target!.Value.StartOffset))
				{
					if (knownProcedures.ContainsKey(nearTargetOffset))
					{
						continue;
					}

					ProcedureEntry discovered = CornerstoneImageLoader.ParseProcedureEntry(image.ObjectBytes, module, nearTargetOffset, image.ObjPath);
					knownProcedures.Add(discovered.StartOffset, discovered);
					discoveredNewProcedure = true;
				}
			}

			if (!discoveredNewProcedure)
			{
				return analyzed.Values.OrderBy(procedure => procedure.Procedure.StartOffset).ToArray();
			}
		}
	}

	/// <summary>
	/// Identifies byte ranges within a module that are not covered by any known procedure.
	/// </summary>
	private static IReadOnlyList<ModuleGap> AnalyzeModuleGaps(byte[] objectBytes, ModuleImage module, IReadOnlyList<ProcedureDisassembly> procedures)
	{
		List<ModuleGap> gaps = new();
		int cursor = 0;

		foreach (ProcedureDisassembly procedure in procedures.OrderBy(entry => entry.Procedure.StartOffset))
		{
			if (procedure.Procedure.StartOffset > cursor)
			{
				gaps.Add(ClassifyGap(objectBytes, module, cursor, procedure.Procedure.StartOffset));
			}

			cursor = Math.Max(cursor, procedure.UpperBound);
		}

		if (cursor < module.Length)
		{
			gaps.Add(ClassifyGap(objectBytes, module, cursor, module.Length));
		}

		return gaps;
	}

	/// <summary>
	/// Classifies a gap region by examining its byte content (zero padding, uniform fill, sparse data, etc.).
	/// </summary>
	private static ModuleGap ClassifyGap(byte[] objectBytes, ModuleImage module, int startOffset, int endOffsetExclusive)
	{
		int length = endOffsetExclusive - startOffset;
		ReadOnlySpan<byte> bytes = objectBytes.AsSpan(module.ObjectOffset + startOffset, length);
		int nonZeroCount = 0;
		HashSet<byte> distinct = new();
		foreach (byte value in bytes)
		{
			if (value != 0)
			{
				nonZeroCount++;
			}

			distinct.Add(value);
		}

		bool endsAtPageBoundary = (endOffsetExclusive & 0xFF) == 0;
		string classification;
		bool likelyPadding;
		if (nonZeroCount == 0)
		{
			classification = endsAtPageBoundary ? "zero padding to page boundary" : "zero padding";
			likelyPadding = true;
		}
		else if (distinct.Count == 1)
		{
			classification = $"uniform byte 0x{bytes[0]:X2}";
			likelyPadding = false;
		}
		else if (nonZeroCount <= 4)
		{
			classification = $"sparse nonzero data ({nonZeroCount} nonzero byte{(nonZeroCount == 1 ? string.Empty : "s")})";
			likelyPadding = false;
		}
		else
		{
			classification = "mixed non-procedure bytes";
			likelyPadding = false;
		}

		string preview = string.Join(" ", bytes[..Math.Min(bytes.Length, 12)].ToArray().Select(value => value.ToString("X2")));
		return new ModuleGap(startOffset, endOffsetExclusive, length, classification, likelyPadding, nonZeroCount, preview);
	}

	private static ProcedureDisassembly AnalyzeProcedure(CornerstoneImage image, InstructionGrammar grammar, ModuleImage module, ProcedureEntry procedure, IReadOnlyList<ProcedureEntry> knownProcedures)
	{
		int upperBound = GetProcedureUpperBound(module, knownProcedures, procedure);
		IReadOnlyList<DecodedInstruction> instructions = BytecodeDecoder.DecodeProcedure(image, grammar, module, procedure, upperBound);
		Dictionary<int, string> labels = BuildLabels(procedure, instructions);
		ProcedureKey key = new(module.ModuleId, procedure.StartOffset);
		CallReference[] outgoingCalls = AnalyzeCalls(image, module, key, knownProcedures, instructions).ToArray();

		return new ProcedureDisassembly(
			key,
			GetProcedureName(procedure),
			module,
			procedure,
			upperBound,
			instructions,
			labels,
			outgoingCalls,
			Array.Empty<CallReference>());
	}

	private static int GetProcedureUpperBound(ModuleImage module, IReadOnlyList<ProcedureEntry> procedures, ProcedureEntry procedure)
	{
		return procedures
			.Where(candidate => candidate.StartOffset > procedure.StartOffset)
			.Select(candidate => candidate.StartOffset)
			.DefaultIfEmpty(module.Length)
			.Min();
	}

	private static Dictionary<int, string> BuildLabels(ProcedureEntry procedure, IReadOnlyList<DecodedInstruction> instructions)
	{
		Dictionary<int, string> labels = new();
		foreach (DecodedInstruction instruction in instructions)
		{
			foreach (DecodedOperand operand in instruction.Operands)
			{
				if (operand.Kind != "jump_target_mixed")
				{
					continue;
				}

				if (operand.RawValue < procedure.CodeOffset)
				{
					continue;
				}

				if (!labels.ContainsKey(operand.RawValue))
				{
					labels.Add(operand.RawValue, $"loc_{operand.RawValue:X4}");
				}
			}
		}

		return labels;
	}

	private static IEnumerable<CallReference> AnalyzeCalls(CornerstoneImage image, ModuleImage module, ProcedureKey source, IReadOnlyList<ProcedureEntry> knownProcedures, IReadOnlyList<DecodedInstruction> instructions)
	{
		for (int index = 0; index < instructions.Count; index++)
		{
			DecodedInstruction instruction = instructions[index];
			if (TryResolveDirectNearCall(module, knownProcedures, instruction, out ProcedureKey nearTarget))
			{
				yield return new CallReference(source, instruction.Offset, instruction.Mnemonic, nearTarget, false, true);
				continue;
			}

			if (TryResolveComputedNearCall(module, knownProcedures, instructions, index, out ProcedureKey computedNearTarget))
			{
				yield return new CallReference(source, instruction.Offset, instruction.Mnemonic, computedNearTarget, false, true);
				continue;
			}

			if (TryResolveDirectFarCall(image, instruction, out ProcedureKey farTarget))
			{
				yield return new CallReference(source, instruction.Offset, instruction.Mnemonic, farTarget, true, true);
				continue;
			}

			if (instruction.Mnemonic.Equals("CALL", StringComparison.OrdinalIgnoreCase))
			{
				yield return new CallReference(source, instruction.Offset, instruction.Mnemonic, null, false, false);
				continue;
			}

			if (instruction.Mnemonic.Equals("CALLF", StringComparison.OrdinalIgnoreCase))
			{
				yield return new CallReference(source, instruction.Offset, instruction.Mnemonic, null, true, false);
			}
		}
	}

	private static bool TryResolveComputedNearCall(ModuleImage module, IReadOnlyList<ProcedureEntry> knownProcedures, IReadOnlyList<DecodedInstruction> instructions, int instructionIndex, out ProcedureKey target)
	{
		target = default;
		DecodedInstruction instruction = instructions[instructionIndex];
		if (!instruction.Mnemonic.Equals("CALL", StringComparison.OrdinalIgnoreCase) || instructionIndex == 0)
		{
			return false;
		}

		DecodedInstruction producer = instructions[instructionIndex - 1];
		if (!TryGetImmediatePushValue(producer, out int targetOffset))
		{
			return false;
		}

		return TryResolveNearTarget(module, knownProcedures, targetOffset, out target);
	}

	private static bool TryResolveDirectNearCall(ModuleImage module, IReadOnlyList<ProcedureEntry> knownProcedures, DecodedInstruction instruction, out ProcedureKey target)
	{
		target = default;
		if (!instruction.Mnemonic.StartsWith("CALL", StringComparison.OrdinalIgnoreCase) || instruction.Mnemonic.StartsWith("CALLF", StringComparison.OrdinalIgnoreCase) || instruction.Mnemonic.Equals("CALL", StringComparison.OrdinalIgnoreCase))
		{
			return false;
		}

		DecodedOperand? operand = instruction.Operands.FirstOrDefault(candidate => candidate.Kind == "near_code_offset_u16le");
		if (operand is null)
		{
			return false;
		}

		return TryResolveNearTarget(module, knownProcedures, operand.RawValue, out target);
	}

	private static bool TryResolveNearTarget(ModuleImage module, IReadOnlyList<ProcedureEntry> knownProcedures, int targetOffset, out ProcedureKey target)
	{
		target = default;
		ProcedureEntry? targetProcedure = knownProcedures.FirstOrDefault(candidate => candidate.StartOffset == targetOffset || candidate.CodeOffset == targetOffset);
		if (targetProcedure is null)
		{
			if (targetOffset < 0 || targetOffset >= module.Length)
			{
				return false;
			}

			target = new ProcedureKey(module.ModuleId, targetOffset);
			return true;
		}

		target = new ProcedureKey(module.ModuleId, targetProcedure.StartOffset);
		return true;
	}

	private static bool TryGetImmediatePushValue(DecodedInstruction instruction, out int value)
	{
		value = 0;
		if (!instruction.Mnemonic.Equals("PUSH", StringComparison.OrdinalIgnoreCase) || instruction.Operands.Count != 1)
		{
			return false;
		}

		DecodedOperand operand = instruction.Operands[0];
		if (operand.Kind != "immediate_u16le")
		{
			return false;
		}

		value = operand.RawValue;
		return true;
	}

	private static bool TryResolveDirectFarCall(CornerstoneImage image, DecodedInstruction instruction, out ProcedureKey target)
	{
		target = default;
		if (!instruction.Mnemonic.StartsWith("CALLF", StringComparison.OrdinalIgnoreCase) || instruction.Mnemonic.Equals("CALLF", StringComparison.OrdinalIgnoreCase))
		{
			return false;
		}

		DecodedOperand? operand = instruction.Operands.FirstOrDefault(candidate => candidate.Kind == "far_proc_selector_u16le");
		if (operand is null)
		{
			return false;
		}

		int moduleId = (operand.RawValue >> 8) & 0xFF;
		int procedureIndex = operand.RawValue & 0xFF;
		if (moduleId < 1 || moduleId > image.Modules.Count)
		{
			return false;
		}

		ModuleImage targetModule = image.Modules[moduleId - 1];
		if (procedureIndex < 0 || procedureIndex >= targetModule.Procedures.Count)
		{
			return false;
		}

		target = new ProcedureKey(moduleId, targetModule.Procedures[procedureIndex].StartOffset);
		return true;
	}

	private static string GetProcedureName(ProcedureEntry procedure)
	{
		return procedure.ExportedProcedureIndex is int exportedProcedureIndex
			? $"Proc{exportedProcedureIndex:000}"
			: $"ProcPriv_{procedure.StartOffset:X4}";
	}
}

/// <summary>
/// The complete result of disassembling a Cornerstone image: all procedures, inter-procedure gaps,
/// and the mapping from exported procedure indices to procedure keys.
/// </summary>
internal sealed record ImageDisassembly(
	IReadOnlyDictionary<ProcedureKey, ProcedureDisassembly> Procedures,
	IReadOnlyDictionary<int, IReadOnlyList<ModuleGap>> ModuleGaps,
	IReadOnlyDictionary<(int ModuleId, int ExportedProcedureIndex), ProcedureKey> ExportedProcedures)
{
	/// <summary>
	/// Gets the disassembly for an exported procedure, identified by module and export index.
	/// </summary>
	/// <exception cref="LinchpinException">Thrown when no matching exported procedure exists.</exception>
	public ProcedureDisassembly GetProcedure(int moduleId, int procedureIndex)
	{
		if (!ExportedProcedures.TryGetValue((moduleId, procedureIndex), out ProcedureKey key))
		{
			throw new LinchpinException($"No exported procedure exists for M{moduleId}:P{procedureIndex}.");
		}

		if (!Procedures.TryGetValue(key, out ProcedureDisassembly? procedure))
		{
			throw new LinchpinException($"No disassembly exists for M{moduleId}:P{procedureIndex}.");
		}

		return procedure;
	}

	/// <summary>
	/// Returns all disassembled procedures in the specified module, ordered by start offset.
	/// </summary>
	public IReadOnlyList<ProcedureDisassembly> GetModuleProcedures(int moduleId)
	{
		return Procedures.Values
			.Where(procedure => procedure.Module.ModuleId == moduleId)
			.OrderBy(procedure => procedure.Procedure.StartOffset)
			.ToArray();
	}

	/// <summary>
	/// Attempts to find an exported procedure's disassembly.
	/// </summary>
	public bool TryGetExportedProcedure(int moduleId, int procedureIndex, out ProcedureDisassembly? procedure)
	{
		procedure = null;
		if (!ExportedProcedures.TryGetValue((moduleId, procedureIndex), out ProcedureKey key))
		{
			return false;
		}

		return Procedures.TryGetValue(key, out procedure);
	}

	/// <summary>
	/// Attempts to find a procedure by its module-relative start offset.
	/// </summary>
	public bool TryGetProcedureByStartOffset(int moduleId, int startOffset, out ProcedureDisassembly? procedure)
	{
		return Procedures.TryGetValue(new ProcedureKey(moduleId, startOffset), out procedure);
	}

	/// <summary>
	/// Formats a procedure key as a human-readable reference, using the procedure name if available.
	/// </summary>
	public string FormatProcedureReference(ProcedureKey key)
	{
		if (!Procedures.TryGetValue(key, out ProcedureDisassembly? procedure))
		{
			return $"M{key.ModuleId}:0x{key.StartOffset:X4}";
		}

		return $"M{key.ModuleId}:{procedure.Name}";
	}
}

/// <summary>
/// A contiguous byte range between procedures that is not covered by any known procedure code.
/// </summary>
internal sealed record ModuleGap(int StartOffset, int EndOffsetExclusive, int Length, string Classification, bool LikelyPadding, int NonZeroCount, string Preview);

/// <summary>
/// Aggregate call statistics for a single module.
/// </summary>
internal sealed record ModuleSummary(int DirectOutgoingCalls, int DirectIncomingCalls, int DynamicCalls, IReadOnlyDictionary<int, int> CrossModuleTargets);

/// <summary>
/// Uniquely identifies a procedure within the image by its module and start offset.
/// </summary>
internal readonly record struct ProcedureKey(int ModuleId, int StartOffset)
{
	public override string ToString() => $"M{ModuleId}:0x{StartOffset:X4}";
}

/// <summary>
/// A fully disassembled procedure: its decoded instructions, jump labels, and call references.
/// </summary>
internal sealed record ProcedureDisassembly(
	ProcedureKey Key,
	string Name,
	ModuleImage Module,
	ProcedureEntry Procedure,
	int UpperBound,
	IReadOnlyList<DecodedInstruction> Instructions,
	IReadOnlyDictionary<int, string> Labels,
	IReadOnlyList<CallReference> OutgoingCalls,
	IReadOnlyList<CallReference> IncomingCalls);

/// <summary>
/// Represents a call site found during disassembly analysis, linking a source procedure to
/// its target (if resolvable) and indicating whether the call is near/far and direct/dynamic.
/// </summary>
internal sealed record CallReference(
	ProcedureKey Source,
	int InstructionOffset,
	string Mnemonic,
	ProcedureKey? Target,
	bool IsFar,
	bool IsDirect);