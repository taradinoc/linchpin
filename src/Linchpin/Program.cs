/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Linchpin;

/// <summary>
/// Entry point for Linchpin, an interpreter and disassembler for the Cornerstone VM.
/// </summary>
internal static class Program
{
	/// <summary>
	/// Application entry point. Parses command-line arguments and dispatches to the appropriate command.
	/// </summary>
	/// <param name="args">Command-line arguments.</param>
	/// <returns>Exit code: 0 for success, non-zero for failure.</returns>
	public static int Main(string[] args)
	{
		return CommandLineOptions.Invoke(args);
	}

	/// <summary>
	/// Runs the selected command (inspect, run, or disassemble) with the given options.
	/// </summary>
	/// <param name="options">Parsed command-line options.</param>
	/// <returns>Exit code.</returns>
	internal static int Run(CommandLineOptions options)
	{
		CornerstoneImage image = CornerstoneImageLoader.Load(options.MmePath, options.ObjPath);
		InstructionGrammar grammar = InstructionGrammar.Load(options.GrammarPath);
		if (options.Command == LinchpinCommand.EmitSource && !string.IsNullOrWhiteSpace(options.OutputPath))
		{
			ExecuteCommand(TextWriter.Null, options, image, grammar);
		}
		else if (string.IsNullOrWhiteSpace(options.OutputPath))
		{
			ExecuteCommand(Console.Out, options, image, grammar);
		}
		else
		{
			using StreamWriter writer = CreateFileWriter(options.OutputPath);
			ExecuteCommand(writer, options, image, grammar);
		}

		return 0;
	}

	/// <summary>
	/// Dispatches execution of the selected command, routing output to the specified writer.
	/// </summary>
	private static void ExecuteCommand(TextWriter writer, CommandLineOptions options, CornerstoneImage image, InstructionGrammar grammar)
	{
		DisassemblySelection selection = new(options.ModuleFilter, options.ProcedureFilter);
		ImageDisassembly? analysis = null;

		if (options.Command == LinchpinCommand.Run)
		{
			VmRunDisplayMode displayMode = ResolveRunDisplayMode(options);
			string inputText = displayMode == VmRunDisplayMode.LiveConsole && !options.HasExplicitInputText
				? string.Empty
				: options.InputText;
			VmExecutionOptions executionOptions = new(
				HostWriteFiles: options.HostWriteFiles,
				AllowInstructionLimitSnapshot: !string.IsNullOrWhiteSpace(options.TestReportPath),
				DataDirectoryPath: options.DataPath);
			VmExecutionResult result = VmExecutor.Run(image, grammar, inputText, displayMode, executionOptions);
			PrintRunResult(writer, result, displayMode);
			if (!string.IsNullOrWhiteSpace(options.TestReportPath))
			{
				VmTestReportWriter.Write(options.TestReportPath!, result);
			}
		}
		else if (options.Command == LinchpinCommand.EmitSource)
		{
			analysis = DisassemblyAnalyzer.Analyze(image, grammar);
			EmitSourceOptions emitSourceOptions = new(
				IncludeStats: options.EmitSourceIncludeStats,
				IncludeOffsets: options.EmitSourceIncludeOffsets,
				IncludePadding: options.EmitSourceIncludePadding);
			if (string.IsNullOrWhiteSpace(options.OutputPath))
			{
				AssemblerSourceEmitter.Write(writer, image, analysis, selection, emitSourceOptions);
			}
			else
			{
				AssemblerSourceEmitter.Write(options.OutputPath!, image, analysis, selection, emitSourceOptions);
			}
		}
		else
		{
			PrintSummary(writer, image, grammar);
		}

		if (!string.IsNullOrWhiteSpace(options.JsonOutputPath))
		{
			analysis ??= DisassemblyAnalyzer.Analyze(image, grammar);
			DisassemblyJsonExporter.Write(options.JsonOutputPath!, image, analysis, selection);
		}
	}

	/// <summary>
	/// Creates a <see cref="StreamWriter"/> for the given output path, creating intermediate directories as needed.
	/// </summary>
	private static StreamWriter CreateFileWriter(string outputPath)
	{
		string fullPath = Path.GetFullPath(outputPath);
		string? directory = Path.GetDirectoryName(fullPath);
		if (!string.IsNullOrWhiteSpace(directory))
		{
			Directory.CreateDirectory(directory);
		}

		return new StreamWriter(fullPath, false);
	}

	/// <summary>
	/// Prints a summary of the loaded Cornerstone image, including module layout and global counts.
	/// </summary>
	private static void PrintSummary(TextWriter writer, CornerstoneImage image, InstructionGrammar grammar)
	{
		VmState initialState = VmBootstrapper.CreateInitialState(image);

		writer.WriteLine($"MME: {image.MmePath}");
		writer.WriteLine($"OBJ: {image.ObjPath}");
		writer.WriteLine($"Header words: {image.HeaderWords.Count}, metadata bytes: {image.MetadataBytes.Length}, object bytes: {image.ObjectBytes.Length}");
		writer.WriteLine($"Entry selector: module {image.EntryPoint.ModuleId}, proc {image.EntryPoint.ProcedureIndex}");
		writer.WriteLine($"Code end: 0x{image.CodeEndOffset:X5}, module headers: 0x{image.ModuleHeaderOffset:X4} ({image.ModuleHeaderLengthWords} words)");
		writer.WriteLine($"Initial RAM: 0x{image.InitialRamOffset:X4}, {image.InitialRamBytes.Length} bytes ({image.InitialRamLengthUnits})");
		writer.WriteLine($"Program globals: {image.Globals.ProgramGlobalCount}, module globals: {string.Join(", ", image.Globals.ModuleGlobalCounts)}");
		writer.WriteLine($"Bootstrap state: module {initialState.CurrentModuleId}, pc 0x{initialState.ProgramCounter:X4}, locals {initialState.CurrentFrame.Locals.Length}, stack depth {initialState.EvaluationStack.Count}");

		int totalProcedures = image.Modules.Sum(module => module.Procedures.Count);
		int proceduresWithInitializers = image.Modules.Sum(module => module.Procedures.Count(procedure => procedure.Header.Initializers.Count > 0));
		int maxLocals = image.Modules.SelectMany(module => module.Procedures).Select(procedure => procedure.Header.LocalCount).DefaultIfEmpty(0).Max();
		int maxHeaderSize = image.Modules.SelectMany(module => module.Procedures).Select(procedure => procedure.Header.HeaderSize).DefaultIfEmpty(0).Max();

		writer.WriteLine($"Modules: {image.Modules.Count}, procedures: {totalProcedures}, with initializers: {proceduresWithInitializers}, max locals: {maxLocals}, max header bytes: {maxHeaderSize}");
		writer.WriteLine();

		foreach (ModuleImage module in image.Modules)
		{
			ProcedureEntry? entryProcedure = image.EntryPoint.ModuleId == module.ModuleId
				? module.Procedures.FirstOrDefault(procedure => procedure.ProcedureIndex == image.EntryPoint.ProcedureIndex)
				: null;

			string entrySuffix = entryProcedure is null
				? string.Empty
				: $", entry code 0x{entryProcedure.CodeOffset:X4}";

			writer.WriteLine(
				$"Module {module.ModuleId,2}: OBJ 0x{module.ObjectOffset:X5}-0x{module.ObjectOffset + module.Length - 1:X5}, procedures {module.Procedures.Count,3}, globals {image.Globals.ModuleGlobalCounts[module.ModuleId - 1],3}{entrySuffix}");
		}

		writer.WriteLine();
		PrintEntryPreview(writer, image, grammar);
	}

	/// <summary>
	/// Prints a short preview of the entry procedure's first few instructions.
	/// </summary>
	private static void PrintEntryPreview(TextWriter writer, CornerstoneImage image, InstructionGrammar grammar)
	{
		ModuleImage module = image.Modules[image.EntryPoint.ModuleId - 1];
		ProcedureEntry procedure = module.Procedures[image.EntryPoint.ProcedureIndex];
		IReadOnlyList<DecodedInstruction> instructions = BytecodeDecoder.DecodeProcedurePreview(image, grammar, module, procedure, 16);

		writer.WriteLine($"Entry procedure preview: module {module.ModuleId}, proc {procedure.ProcedureIndex}, start 0x{procedure.StartOffset:X4}, code 0x{procedure.CodeOffset:X4}");
		foreach (DecodedInstruction instruction in instructions)
		{
			string operands = instruction.Operands.Count == 0
				? string.Empty
				: " " + string.Join(", ", instruction.Operands.Select(operand => operand.DisplayText));

			writer.WriteLine($"  0x{instruction.Offset:X4}: {instruction.Mnemonic}{operands}");
		}
	}

	/// <summary>
	/// Determines the display mode for a run command based on output redirection and test-report settings.
	/// </summary>
	private static VmRunDisplayMode ResolveRunDisplayMode(CommandLineOptions options)
	{
		if (!string.IsNullOrWhiteSpace(options.TestReportPath))
		{
			return VmRunDisplayMode.Transcript;
		}

		if (!string.IsNullOrWhiteSpace(options.OutputPath))
		{
			return VmRunDisplayMode.Transcript;
		}

		return Console.IsInputRedirected || Console.IsOutputRedirected
			? VmRunDisplayMode.Transcript
			: VmRunDisplayMode.LiveConsole;
	}

	/// <summary>
	/// Prints the result of a VM execution run, including halt code, instruction count, and screen transcript.
	/// </summary>
	private static void PrintRunResult(TextWriter writer, VmExecutionResult result, VmRunDisplayMode displayMode)
	{
		if (result.StopReason == VmExecutionStopReason.InstructionLimit)
		{
			writer.WriteLine("Execution stopped at instruction limit before HALT.");
		}
		else if (result.StopReason == VmExecutionStopReason.OutputPattern)
		{
			writer.WriteLine($"Execution stopped after matching {result.StopDetail}.");
		}
		else if (result.StopReason == VmExecutionStopReason.KbInputCount)
		{
			writer.WriteLine($"Execution stopped after reaching {result.StopDetail}.");
		}

		writer.WriteLine($"Halt code: 0x{result.HaltCode:X4}");
		writer.WriteLine($"Executed instructions: {result.ExecutedInstructionCount}");
		if (displayMode == VmRunDisplayMode.LiveConsole)
		{
			// writer.WriteLine("Screen transcript omitted because live console rendering was active.");
			return;
		}

		writer.WriteLine("Screen transcript:");
		writer.WriteLine(result.ScreenText.Length == 0 ? "<empty>" : result.ScreenText);
	}
}
