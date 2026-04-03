/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Linchpin;

/// <summary>
/// Emits Chisel-compatible assembler source (<c>.cas</c>) from a disassembled Cornerstone image.
/// Supports single-file output or multi-file output with one file per module.
/// </summary>
internal static class AssemblerSourceEmitter
{
	/// <summary>
	/// Writes assembler source for the selected modules and procedures to a <see cref="TextWriter"/>.
	/// Always produces a single combined document.
	/// </summary>
	public static void Write(TextWriter writer, CornerstoneImage image, ImageDisassembly analysis, DisassemblySelection selection, EmitSourceOptions options)
	{
		SelectionContext context = BuildSelectionContext(image, analysis, selection);
		WriteMainFile(writer, image, analysis, context, options, splitModules: false);
	}

	/// <summary>
	/// Writes assembler source to a file at the specified path. When multiple modules are selected,
	/// writes a main file with <c>.include</c> directives and separate per-module files.
	/// </summary>
	public static void Write(string outputPath, CornerstoneImage image, ImageDisassembly analysis, DisassemblySelection selection, EmitSourceOptions options)
	{
		SelectionContext context = BuildSelectionContext(image, analysis, selection);
		string fullPath = Path.GetFullPath(outputPath);
		string directory = Path.GetDirectoryName(fullPath) ?? Directory.GetCurrentDirectory();
		Directory.CreateDirectory(directory);

		if (context.Modules.Count <= 1)
		{
			using StreamWriter writer = new(fullPath, false);
			WriteMainFile(writer, image, analysis, context, options, splitModules: false);
			return;
		}

		using (StreamWriter writer = new(fullPath, false))
		{
			WriteMainFile(writer, image, analysis, context, options, splitModules: true);
		}

		foreach (ModuleImage module in context.Modules)
		{
			string modulePath = Path.Combine(directory, GetModuleFileName(module.ModuleId));
			using StreamWriter writer = new(modulePath, false);
			WriteModuleFile(writer, image, module, analysis, context.ModuleProcedures[module.ModuleId], options);
		}
	}

	private static SelectionContext BuildSelectionContext(CornerstoneImage image, ImageDisassembly analysis, DisassemblySelection selection)
	{
		List<ModuleImage> selectedModules = image.Modules.Where(selection.IncludesModule).ToList();
		if (selectedModules.Count == 0)
		{
			throw new LinchpinException($"Selection '{selection.Describe()}' did not match any modules.");
		}

		Dictionary<int, IReadOnlyList<ProcedureDisassembly>> moduleProcedures = selectedModules.ToDictionary(
			module => module.ModuleId,
			module => (IReadOnlyList<ProcedureDisassembly>)analysis.GetModuleProcedures(module.ModuleId)
				.Where(selection.IncludesProcedure)
				.OrderBy(procedure => procedure.Procedure.StartOffset)
				.ToArray());

		List<ProcedureDisassembly> selectedProcedures = moduleProcedures.Values.SelectMany(procedures => procedures).ToList();
		if (selectedProcedures.Count == 0)
		{
			throw new LinchpinException($"Selection '{selection.Describe()}' did not match any procedures.");
		}

		ProcedureDisassembly exportedEntryProcedure = analysis.GetProcedure(image.EntryPoint.ModuleId, image.EntryPoint.ProcedureIndex);
		ProcedureDisassembly entryProcedure = selectedProcedures.FirstOrDefault(procedure => procedure.Key == exportedEntryProcedure.Key)
			?? selectedProcedures[0];

		return new SelectionContext(
			selectedModules,
			selectedModules.Select(module => module.ModuleId).ToHashSet(),
			moduleProcedures,
			selectedProcedures,
			entryProcedure);
	}

	private static void WriteMainFile(TextWriter writer, CornerstoneImage image, ImageDisassembly analysis, SelectionContext context, EmitSourceOptions options, bool splitModules)
	{
		IReadOnlyDictionary<int, ModuleSummary> moduleSummaries = options.IncludeStats
			? BuildModuleSummaries(analysis)
			: EmptyModuleSummaryMap.Instance;

		writer.WriteLine($"; Conservative source emitted from {Path.GetFileName(image.ObjPath)}");
		writer.WriteLine($"; Selection: {DescribeSelection(context)}");
		writer.WriteLine($"; Original entry: {GetModuleName(image.EntryPoint.ModuleId)}.{GetExportProcedureName(image.EntryPoint.ProcedureIndex)}");
		writer.WriteLine();

		EmitWordDirective(writer, ".program_globals", image.Globals.ProgramGlobals.Select(value => FormatWord(value)).ToList(), 12);
		writer.WriteLine();

		if (splitModules)
		{
			foreach (ModuleImage module in context.Modules)
			{
				writer.WriteLine($".include \"{GetModuleFileName(module.ModuleId)}\"");
			}

			writer.WriteLine();
			writer.WriteLine($".entry {GetModuleName(context.EntryProcedure.Key.ModuleId)}.{context.EntryProcedure.Name}");
			writer.WriteLine();
			EmitRam(writer, image.InitialRamBytes);
		}
		else
		{
			EmitRam(writer, image.InitialRamBytes);

			foreach (ModuleImage module in context.Modules)
			{
				EmitModule(
					writer,
					image,
					module,
					analysis,
					context.ModuleProcedures[module.ModuleId],
					context.AvailableModuleIds,
					moduleSummaries,
					options);
			}

			writer.WriteLine($".entry {GetModuleName(context.EntryProcedure.Key.ModuleId)}.{context.EntryProcedure.Name}");
		}
	}

	private static void WriteModuleFile(TextWriter writer, CornerstoneImage image, ModuleImage module, ImageDisassembly analysis, IReadOnlyList<ProcedureDisassembly> procedures, EmitSourceOptions options)
	{
		IReadOnlyDictionary<int, ModuleSummary> moduleSummaries = options.IncludeStats
			? BuildModuleSummaries(analysis)
			: EmptyModuleSummaryMap.Instance;

		writer.WriteLine($"; Module source for {GetModuleName(module.ModuleId)}");
		writer.WriteLine();
		EmitModule(
			writer,
			image,
			module,
			analysis,
			procedures,
			analysis.Procedures.Values.Select(procedure => procedure.Module.ModuleId).ToHashSet(),
			moduleSummaries,
			options);
	}

	private static void EmitModule(
		TextWriter writer,
		CornerstoneImage image,
		ModuleImage module,
		ImageDisassembly analysis,
		IReadOnlyList<ProcedureDisassembly> procedures,
		IReadOnlySet<int> availableModuleIds,
		IReadOnlyDictionary<int, ModuleSummary> moduleSummaries,
		EmitSourceOptions options)
	{
		if (options.IncludeStats && moduleSummaries.TryGetValue(module.ModuleId, out ModuleSummary? summary))
		{
			IReadOnlyList<ProcedureDisassembly> allModuleProcedures = analysis.GetModuleProcedures(module.ModuleId);
			int privateProcedureCount = allModuleProcedures.Count(procedure => !procedure.Procedure.IsExported);
			writer.WriteLine($"; Module {module.ModuleId}  ; OBJ 0x{module.ObjectOffset:X5}-0x{module.ObjectOffset + module.Length - 1:X5}, exports {module.Procedures.Count}, discovered private {privateProcedureCount}, globals {image.Globals.ModuleGlobalCounts[module.ModuleId - 1]}");
			writer.WriteLine($"; direct calls out {summary.DirectOutgoingCalls}, direct callers in {summary.DirectIncomingCalls}, dynamic call sites {summary.DynamicCalls}");
			if (summary.CrossModuleTargets.Count > 0)
			{
				writer.WriteLine($"; cross-module direct calls {FormatCrossModuleTargets(summary.CrossModuleTargets)}");
			}

			IReadOnlyList<ModuleGap> gaps = analysis.ModuleGaps[module.ModuleId];
			if (gaps.Count > 0)
			{
				writer.WriteLine($"; uncovered ranges {FormatModuleGaps(gaps)}");
			}
		}

		writer.WriteLine($".module {GetModuleName(module.ModuleId)}");
		EmitWordDirective(
			writer,
			".module_globals",
			image.Globals.ModuleGlobals[module.ModuleId - 1].Select(value => FormatWord(value)).ToArray(),
			12);

		foreach (ProcedureDisassembly exportedProcedure in procedures
			.Where(procedure => procedure.Procedure.ExportedProcedureIndex is not null)
			.OrderBy(procedure => procedure.Procedure.ExportedProcedureIndex))
		{
			writer.WriteLine($".export {exportedProcedure.Name}, {exportedProcedure.Procedure.ExportedProcedureIndex}");
		}

		writer.WriteLine();

		foreach (ProcedureDisassembly procedure in procedures)
		{
			EmitProcedure(writer, procedure, analysis, availableModuleIds, options);
		}
	}

	private static void EmitProcedure(TextWriter writer, ProcedureDisassembly procedure, ImageDisassembly analysis, IReadOnlySet<int> availableModuleIds, EmitSourceOptions options)
	{
		writer.WriteLine($".proc {procedure.Name} locals {procedure.Procedure.Header.LocalCount}");
		writer.WriteLine($"; original offsets: start 0x{procedure.Procedure.StartOffset:X4}, code 0x{procedure.Procedure.CodeOffset:X4}, end 0x{procedure.UpperBound:X4}");
		if (options.IncludeStats)
		{
			if (procedure.OutgoingCalls.Count > 0)
			{
				writer.WriteLine($"; calls {FormatCallList(procedure.OutgoingCalls, analysis)}");
			}

			if (procedure.IncomingCalls.Count > 0)
			{
				writer.WriteLine($"; called by {FormatCallerList(procedure.IncomingCalls, analysis)}");
			}
		}

		foreach (ProcedureInitializer initializer in procedure.Procedure.Header.Initializers)
		{
			writer.WriteLine($".init {initializer.LocalIndex}, {FormatWord(initializer.Value)}");
		}

		IReadOnlyList<DecodedInstruction> instructions = FilterInstructionsForEmission(procedure, options);
		foreach (DecodedInstruction instruction in instructions)
		{
			if (options.IncludeOffsets)
			{
				writer.WriteLine($"off_{instruction.Offset:X4}:");
			}

			if (procedure.Labels.TryGetValue(instruction.Offset, out string? label))
			{
				writer.WriteLine($"{label}:");
			}

			string mnemonic = FormatSourceMnemonic(instruction);
			string operands = instruction.Operands.Count == 0
				? string.Empty
				: " " + string.Join(", ", instruction.Operands.Select(operand => FormatSourceOperand(procedure, analysis, operand, availableModuleIds)));
			writer.WriteLine($"    {mnemonic}{operands}");
		}

		writer.WriteLine(".endproc");
		writer.WriteLine();
	}

	private static IReadOnlyList<DecodedInstruction> FilterInstructionsForEmission(ProcedureDisassembly procedure, EmitSourceOptions options)
	{
		if (options.IncludePadding)
		{
			return procedure.Instructions;
		}

		IReadOnlyList<DecodedInstruction> instructions = procedure.Instructions;
		int endExclusive = instructions.Count;
		while (endExclusive > 0 && instructions[endExclusive - 1].Mnemonic.Equals("BREAK", StringComparison.OrdinalIgnoreCase))
		{
			endExclusive--;
		}

		List<DecodedInstruction> filtered = new(capacity: endExclusive);
		int index = 0;
		while (index < endExclusive)
		{
			DecodedInstruction instruction = instructions[index];
			if (instruction.Mnemonic.Equals("NEXTB", StringComparison.OrdinalIgnoreCase))
			{
				index++;
				while (index < endExclusive)
				{
					DecodedInstruction candidate = instructions[index];
					if (!candidate.Mnemonic.Equals("BREAK", StringComparison.OrdinalIgnoreCase) || IsBlockStart(procedure, candidate.Offset))
					{
						break;
					}

					index++;
				}

				continue;
			}

			filtered.Add(instruction);
			index++;
		}

		return filtered;
	}

	private static bool IsBlockStart(ProcedureDisassembly procedure, int offset)
	{
		if (offset == procedure.Procedure.CodeOffset)
		{
			return true;
		}

		return procedure.Labels.ContainsKey(offset);
	}

	private static void EmitRam(TextWriter writer, byte[] initialRamBytes)
	{
		if (initialRamBytes.Length == 0)
		{
			return;
		}

		writer.WriteLine(".ramorg 0x0000");
		const int chunkSize = 16;
		for (int offset = 0; offset < initialRamBytes.Length; offset += chunkSize)
		{
			int length = Math.Min(chunkSize, initialRamBytes.Length - offset);
			List<string> values = initialRamBytes.AsSpan(offset, length).ToArray().Select(value => $"0x{value:X2}").ToList();
			writer.WriteLine($"RAM_{offset / 2:X4}::");
			writer.WriteLine($".byte {string.Join(", ", values)}");
		}

		writer.WriteLine();
	}

	private static void EmitWordDirective(TextWriter writer, string directive, IReadOnlyList<string> tokens, int perLine)
	{
		if (tokens.Count == 0)
		{
			return;
		}

		for (int index = 0; index < tokens.Count; index += perLine)
		{
			writer.WriteLine($"{directive} {string.Join(", ", tokens.Skip(index).Take(perLine))}");
		}
	}

	private static string FormatSourceOperand(ProcedureDisassembly procedure, ImageDisassembly analysis, DecodedOperand operand, IReadOnlySet<int> availableModuleIds)
	{
		return operand.Kind switch
		{
			"jump_target_mixed" => FormatJumpOperand(procedure, operand),
			"near_code_offset_u16le" => TryFormatNearProcedureOperand(procedure, analysis, operand, out string? nearProcedureName) ? nearProcedureName! : $"0x{operand.RawValue:X4}",
			"packed_code_location_u16le" => $"0x{operand.RawValue:X4}",
			"far_proc_selector_u16le" => TryFormatFarProcedureOperand(analysis, operand, availableModuleIds, out string? farProcedureName) ? farProcedureName! : $"0x{operand.RawValue:X4}",
			"local_index_u8" or "aggregate_byte_index_u8" or "aggregate_word_index_u8"
				or "argument_count_u8" or "return_count_u8" or "stack_drop_count_u8" => operand.DisplayText,
			"immediate_u8" or "immediate_u16le" or "status_code_u16le" or "bitfield_control_u16le" => operand.DisplayText,
			"open_mode_u8" or "program_global_index_u8" or "module_global_index_u8"
				or "display_subop_u8" or "display_ext_subop_u8" => $"0x{operand.RawValue:X2}",
			_ => operand.DisplayText,
		};
	}

	private static string FormatSourceMnemonic(DecodedInstruction instruction) => instruction.Mnemonic;

	private static string FormatJumpOperand(ProcedureDisassembly procedure, DecodedOperand operand)
	{
		if (operand.DisplayText.Contains("(abs", StringComparison.OrdinalIgnoreCase))
		{
			return $"ABS:0x{operand.RawValue:X4}";
		}

		bool hasInstructionBoundary = procedure.Instructions.Any(instruction => instruction.Offset == operand.RawValue);
		string target = hasInstructionBoundary && procedure.Labels.TryGetValue(operand.RawValue, out string? label)
			? label
			: $"0x{operand.RawValue:X4}";
		return $"REL:{target}";
	}

	private static bool TryFormatNearProcedureOperand(ProcedureDisassembly procedure, ImageDisassembly analysis, DecodedOperand operand, out string? name)
	{
		name = null;
		if (!analysis.TryGetProcedureByStartOffset(procedure.Module.ModuleId, operand.RawValue, out ProcedureDisassembly? targetProcedure) || targetProcedure is null)
		{
			return false;
		}

		name = targetProcedure.Name;
		return true;
	}

	private static bool TryFormatFarProcedureOperand(ImageDisassembly analysis, DecodedOperand operand, IReadOnlySet<int> availableModuleIds, out string? name)
	{
		name = null;
		int moduleId = (operand.RawValue >> 8) & 0xFF;
		int procedureIndex = operand.RawValue & 0xFF;
		if (!availableModuleIds.Contains(moduleId))
		{
			return false;
		}

		if (!analysis.TryGetExportedProcedure(moduleId, procedureIndex, out ProcedureDisassembly? targetProcedure) || targetProcedure is null)
		{
			return false;
		}

		name = $"{GetModuleName(moduleId)}.{targetProcedure.Name}";
		return true;
	}

	private static string DescribeSelection(SelectionContext context) => context.Modules.Count == 1
		? $"module {context.Modules[0].ModuleId}"
		: $"{context.Modules.Count} modules";

	private static Dictionary<int, ModuleSummary> BuildModuleSummaries(ImageDisassembly analysis)
	{
		Dictionary<int, ModuleSummary> summaries = new();
		foreach (IGrouping<int, ProcedureDisassembly> group in analysis.Procedures.Values.GroupBy(procedure => procedure.Module.ModuleId))
		{
			int directOutgoingCalls = group.Sum(procedure => procedure.OutgoingCalls.Count(call => call.IsDirect));
			int directIncomingCalls = group.Sum(procedure => procedure.IncomingCalls.Count);
			int dynamicCalls = group.Sum(procedure => procedure.OutgoingCalls.Count(call => !call.IsDirect));
			Dictionary<int, int> crossModuleTargets = group
				.SelectMany(procedure => procedure.OutgoingCalls)
				.Where(call => call.Target is ProcedureKey target && target.ModuleId != group.Key)
				.GroupBy(call => call.Target!.Value.ModuleId)
				.ToDictionary(targetGroup => targetGroup.Key, targetGroup => targetGroup.Count());

			summaries.Add(group.Key, new ModuleSummary(directOutgoingCalls, directIncomingCalls, dynamicCalls, crossModuleTargets));
		}

		return summaries;
	}

	private static string FormatCallList(IReadOnlyList<CallReference> calls, ImageDisassembly analysis)
	{
		List<string> entries = calls
			.GroupBy(call => call.Target)
			.OrderBy(group => group.Key?.ModuleId ?? int.MaxValue)
			.ThenBy(group => group.Key?.StartOffset ?? int.MaxValue)
			.Select(group =>
			{
				if (group.Key is ProcedureKey key)
				{
					string formatted = analysis.FormatProcedureReference(key);
					return group.Count() == 1 ? formatted : $"{formatted} x{group.Count()}";
				}

				CallReference sample = group.First();
				string label = sample.IsFar ? "dynamic far" : "dynamic near";
				return group.Count() == 1 ? label : $"{label} x{group.Count()}";
			})
			.ToList();

		return FormatListWithOverflow(entries, 8);
	}

	private static string FormatCallerList(IReadOnlyList<CallReference> calls, ImageDisassembly analysis)
	{
		List<string> entries = calls
			.GroupBy(call => call.Source)
			.OrderBy(group => group.Key.ModuleId)
			.ThenBy(group => group.Key.StartOffset)
			.Select(group =>
			{
				string formatted = analysis.FormatProcedureReference(group.Key);
				return group.Count() == 1 ? formatted : $"{formatted} x{group.Count()}";
			})
			.ToList();

		return FormatListWithOverflow(entries, 8);
	}

	private static string FormatListWithOverflow(IReadOnlyList<string> entries, int maxEntries)
	{
		if (entries.Count <= maxEntries)
		{
			return string.Join(", ", entries);
		}

		return string.Join(", ", entries.Take(maxEntries)) + $", ... +{entries.Count - maxEntries} more";
	}

	private static string FormatCrossModuleTargets(IReadOnlyDictionary<int, int> crossModuleTargets)
	{
		return string.Join(", ", crossModuleTargets.OrderBy(entry => entry.Key).Select(entry => $"M{entry.Key} x{entry.Value}"));
	}

	private static string FormatModuleGaps(IReadOnlyList<ModuleGap> gaps)
	{
		List<string> entries = gaps
			.Select(gap => $"0x{gap.StartOffset:X4}-0x{gap.EndOffsetExclusive - 1:X4} ({gap.Length} bytes, {gap.Classification})")
			.ToList();

		return FormatListWithOverflow(entries, 6);
	}

	private static string GetModuleName(int moduleId) => $"Module{moduleId:00}";
	private static string GetModuleFileName(int moduleId) => $"{GetModuleName(moduleId)}.cas";
	private static string GetExportProcedureName(int procedureIndex) => $"Proc{procedureIndex:000}";
	private static string FormatWord(ushort value) => $"0x{value:X4}";

	private sealed record SelectionContext(
		IReadOnlyList<ModuleImage> Modules,
		IReadOnlySet<int> AvailableModuleIds,
		IReadOnlyDictionary<int, IReadOnlyList<ProcedureDisassembly>> ModuleProcedures,
		IReadOnlyList<ProcedureDisassembly> Procedures,
		ProcedureDisassembly EntryProcedure);

	private sealed class EmptyModuleSummaryMap : IReadOnlyDictionary<int, ModuleSummary>
	{
		public static readonly EmptyModuleSummaryMap Instance = new();

		public IEnumerable<int> Keys => [];
		public IEnumerable<ModuleSummary> Values => [];
		public int Count => 0;
		public ModuleSummary this[int key] => throw new KeyNotFoundException();

		public bool ContainsKey(int key) => false;
		public bool TryGetValue(int key, out ModuleSummary value)
		{
			value = null!;
			return false;
		}

		public IEnumerator<KeyValuePair<int, ModuleSummary>> GetEnumerator()
		{
			yield break;
		}

		System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator() => GetEnumerator();
	}
}

/// <summary>
/// Options controlling assembler source output: whether to include statistics comments, per-instruction
/// offset labels, and NEXTB/BREAK padding instructions.
/// </summary>
internal sealed record EmitSourceOptions(bool IncludeStats, bool IncludeOffsets, bool IncludePadding);
