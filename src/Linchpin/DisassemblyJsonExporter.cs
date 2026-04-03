/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Text.Json;

namespace Linchpin;

/// <summary>
/// Exports a disassembly analysis as structured JSON, including module summaries,
/// procedure details, call graphs, and per-instruction data.
/// </summary>
internal static class DisassemblyJsonExporter
{
	/// <summary>
	/// Writes the disassembly analysis to a JSON file at the specified path.
	/// </summary>
	/// <param name="path">The output file path.</param>
	/// <param name="image">The loaded Cornerstone image.</param>
	/// <param name="analysis">The disassembly analysis to export.</param>
	/// <param name="selection">The module/procedure filter controlling which data is included.</param>
	public static void Write(string path, CornerstoneImage image, ImageDisassembly analysis, DisassemblySelection selection)
	{
		string fullPath = Path.GetFullPath(path);
		string? directory = Path.GetDirectoryName(fullPath);
		if (!string.IsNullOrWhiteSpace(directory))
		{
			Directory.CreateDirectory(directory);
		}

		Dictionary<int, ModuleSummary> moduleSummaries = BuildModuleSummaries(analysis);
		List<object> modules = image.Modules
			.Where(selection.IncludesModule)
			.Select(module => new
			{
				moduleId = module.ModuleId,
				objectOffset = module.ObjectOffset,
				length = module.Length,
				globalCount = image.Globals.ModuleGlobalCounts[module.ModuleId - 1],
				summary = moduleSummaries[module.ModuleId],
				gaps = analysis.ModuleGaps[module.ModuleId].Select(gap => new
				{
					startOffset = gap.StartOffset,
					endOffsetExclusive = gap.EndOffsetExclusive,
					length = gap.Length,
					classification = gap.Classification,
					likelyPadding = gap.LikelyPadding,
					nonZeroCount = gap.NonZeroCount,
					preview = gap.Preview,
				}),
				procedures = analysis.GetModuleProcedures(module.ModuleId)
					.Where(selection.IncludesProcedure)
					.Select(procedure => new
					{
						name = procedure.Name,
						exportedProcedureIndex = procedure.Procedure.ExportedProcedureIndex,
						startOffset = procedure.Procedure.StartOffset,
						codeOffset = procedure.Procedure.CodeOffset,
						upperBound = procedure.UpperBound,
						localCount = procedure.Procedure.Header.LocalCount,
						initializers = procedure.Procedure.Header.Initializers.Select(initializer => new
						{
							localIndex = initializer.LocalIndex,
							value = initializer.Value,
							isByteEncoded = initializer.IsByteEncoded,
						}),
						labels = procedure.Labels.OrderBy(label => label.Key).Select(label => new
						{
							offset = label.Key,
							name = label.Value,
						}),
						outgoingCalls = procedure.OutgoingCalls.Select(call => new
						{
							source = call.Source.ToString(),
							instructionOffset = call.InstructionOffset,
							mnemonic = call.Mnemonic,
							target = call.Target?.ToString(),
							isFar = call.IsFar,
							isDirect = call.IsDirect,
						}),
						incomingCalls = procedure.IncomingCalls.Select(call => new
						{
							source = call.Source.ToString(),
							instructionOffset = call.InstructionOffset,
							mnemonic = call.Mnemonic,
							target = call.Target?.ToString(),
							isFar = call.IsFar,
							isDirect = call.IsDirect,
						}),
						instructions = procedure.Instructions.Select(instruction => new
						{
							offset = instruction.Offset,
							byteLength = instruction.ByteLength,
							mnemonic = instruction.Mnemonic,
							operands = instruction.Operands.Select(operand => new
							{
								kind = operand.Kind,
								rawValue = operand.RawValue,
								display = operand.DisplayText,
							}),
						}),
					}),
			})
			.Cast<object>()
			.ToList();

		object document = new
		{
			mmePath = image.MmePath,
			objPath = image.ObjPath,
			entry = image.EntryPoint,
			programGlobalCount = image.Globals.ProgramGlobalCount,
			initialRamBytes = image.InitialRamBytes.Length,
			selection = selection.Describe(),
			modules,
		};

		JsonSerializerOptions options = new() { WriteIndented = true };
		File.WriteAllText(fullPath, JsonSerializer.Serialize(document, options));
	}

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
}