/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Diagnostics;
using System.Text;
using System.Text.Json;

namespace Linchpin.TestCaseTool;

internal static partial class Program
{
	public static int Main(string[] args)
	{
		try
		{
			Options options = Options.Parse(args);
			ManifestStore manifestStore = new(RepositoryLayout.Root);
			ManifestCase? existingCase = manifestStore.TryGetCase(options.Name);
			if (existingCase is not null && !options.UpdateExisting)
			{
				throw new ToolException($"A runtime case named '{options.Name}' already exists. Use --update to overwrite it.");
			}

			string? chiselSourcePathForManifest = !string.IsNullOrWhiteSpace(options.SourcePath)
				? ToRepoRelativePath(options.SourcePath)
				: existingCase?.ChiselSourcePath;
			string? mmePathForManifest = chiselSourcePathForManifest is null
				? (!string.IsNullOrWhiteSpace(options.MmePath)
					? ToRepoRelativePath(options.MmePath)
					: existingCase?.MmePath)
				: null;
			string? objPathForManifest = chiselSourcePathForManifest is null
				? (!string.IsNullOrWhiteSpace(options.ObjPath)
					? ToRepoRelativePath(options.ObjPath)
					: existingCase?.ObjPath)
				: null;
			string? dataDirectoryPathForManifest = !string.IsNullOrWhiteSpace(options.DataDirectoryPath)
				? ToRepoRelativePath(options.DataDirectoryPath)
				: existingCase?.DataDirectoryPath;
			string? resolvedDataDirectoryPath = !string.IsNullOrWhiteSpace(options.DataDirectoryPath)
				? options.DataDirectoryPath
				: ResolveOptionalRepoPath(existingCase?.DataDirectoryPath);

			InterpreterRunner primaryRunner = InterpreterRunner.Create(options.TestedInterpreter, RepositoryLayout.Root);
			InterpreterRunner secondaryRunner = InterpreterRunner.Create(options.TestedInterpreter == InterpreterKind.Linchpin ? InterpreterKind.LinchpinSt : InterpreterKind.Linchpin, RepositoryLayout.Root);

			RuntimeExecutionRequest request = new(
				options.Name,
				string.IsNullOrWhiteSpace(options.MmePath) ? null : Path.GetFullPath(options.MmePath),
				string.IsNullOrWhiteSpace(options.ObjPath) ? null : Path.GetFullPath(options.ObjPath),
				string.IsNullOrWhiteSpace(options.SourcePath) ? null : Path.GetFullPath(options.SourcePath),
				string.IsNullOrWhiteSpace(resolvedDataDirectoryPath) ? null : Path.GetFullPath(resolvedDataDirectoryPath),
				options.InputText,
				options.InstructionLimit);

			ToolRunOutcome primaryOutcome = primaryRunner.Run(request);
			if (primaryOutcome.Process.ExitCode != 0)
			{
				throw new ToolException(DescribeProcessFailure(primaryRunner.DisplayName, primaryOutcome.Process));
			}

			string expectedScreenRelativePath = manifestStore.WriteScreenFixture(options.Name, primaryOutcome.Report.ScreenText);
			ManifestCase savedCase = manifestStore.UpsertCase(new ManifestCase(
				options.Name,
				mmePathForManifest,
				objPathForManifest,
				chiselSourcePathForManifest,
				dataDirectoryPathForManifest,
				options.InputText,
				primaryOutcome.Report.ExecutedInstructionCount,
				options.InstructionLimit,
				expectedScreenRelativePath));

			Console.WriteLine($"Saved runtime case '{savedCase.Name}'.");
			Console.WriteLine($"Interpreter: {primaryRunner.DisplayName}");
			if (!string.IsNullOrWhiteSpace(savedCase.ChiselSourcePath))
			{
				Console.WriteLine($"Source: {savedCase.ChiselSourcePath}");
			}

			if (!string.IsNullOrWhiteSpace(savedCase.MmePath))
			{
				Console.WriteLine($"MME: {savedCase.MmePath}");
			}

			if (!string.IsNullOrWhiteSpace(savedCase.ObjPath))
			{
				Console.WriteLine($"OBJ: {savedCase.ObjPath}");
			}

			if (savedCase.InstructionLimit.HasValue)
			{
				Console.WriteLine($"Instruction limit: {savedCase.InstructionLimit.Value}");
			}

			Console.WriteLine($"Expected instruction count: {savedCase.ExpectedInstructionCount}");
			Console.WriteLine($"Expected screen: {savedCase.ExpectedScreenPath}");

			ToolRunOutcome? secondaryOutcome = null;
			try
			{
				secondaryOutcome = secondaryRunner.Run(request);
			}
			catch (ToolException exception)
			{
				Console.Error.WriteLine($"Warning: unable to verify parity with {secondaryRunner.DisplayName}: {exception.Message}");
			}

			if (secondaryOutcome is not null)
			{
				if (secondaryOutcome.Process.ExitCode != 0)
				{
					Console.Error.WriteLine($"Warning: {secondaryRunner.DisplayName} failed while checking parity.");
					Console.Error.WriteLine(DescribeProcessFailure(secondaryRunner.DisplayName, secondaryOutcome.Process));
				}
				else
				{
					ReportParity(primaryOutcome.Report, secondaryOutcome.Report, secondaryRunner.DisplayName);
				}
			}

			return 0;
		}
		catch (ToolException exception)
		{
			Console.Error.WriteLine(exception.Message);
			PrintUsage();
			return 1;
		}
	}

	private static void ReportParity(ToolRuntimeReport primary, ToolRuntimeReport secondary, string secondaryDisplayName)
	{
		string normalizedPrimaryScreen = NormalizeScreenText(primary.ScreenText);
		string normalizedSecondaryScreen = NormalizeScreenText(secondary.ScreenText);
		bool screensMatch = string.Equals(normalizedPrimaryScreen, normalizedSecondaryScreen, StringComparison.Ordinal);

		if (primary.HaltCode == secondary.HaltCode
			&& primary.ExecutedInstructionCount == secondary.ExecutedInstructionCount
			&& screensMatch)
		{
			Console.WriteLine($"Parity check passed with {secondaryDisplayName}.");
			return;
		}

		Console.Error.WriteLine($"Warning: {secondaryDisplayName} does not match the saved interpreter result.");
		if (primary.HaltCode != secondary.HaltCode)
		{
			Console.Error.WriteLine($"  Halt code differs: saved={primary.HaltCode}, {secondaryDisplayName}={secondary.HaltCode}");
		}

		if (primary.ExecutedInstructionCount != secondary.ExecutedInstructionCount)
		{
			Console.Error.WriteLine($"  Instruction count differs: saved={primary.ExecutedInstructionCount}, {secondaryDisplayName}={secondary.ExecutedInstructionCount}");
		}

		if (!screensMatch)
		{
			Console.Error.WriteLine("  Screen text differs.");
		}

		Console.Error.WriteLine("Diff:");
		Console.Error.WriteLine(BuildParityDiff(primary, secondary, secondaryDisplayName, screensMatch, normalizedPrimaryScreen, normalizedSecondaryScreen));
	}

	private static string BuildParityDiff(
		ToolRuntimeReport primary,
		ToolRuntimeReport secondary,
		string secondaryDisplayName,
		bool screensMatch,
		string normalizedPrimaryScreen,
		string normalizedSecondaryScreen)
	{
		StringBuilder builder = new();
		builder.AppendLine("--- saved");
		builder.AppendLine($"+++ {secondaryDisplayName}");

		if (primary.HaltCode != secondary.HaltCode)
		{
			builder.AppendLine("@@ haltCode @@");
			builder.AppendLine($"- {primary.HaltCode}");
			builder.AppendLine($"+ {secondary.HaltCode}");
		}

		if (primary.ExecutedInstructionCount != secondary.ExecutedInstructionCount)
		{
			builder.AppendLine("@@ executedInstructionCount @@");
			builder.AppendLine($"- {primary.ExecutedInstructionCount}");
			builder.AppendLine($"+ {secondary.ExecutedInstructionCount}");
		}

		if (!screensMatch)
		{
			builder.Append(BuildScreenDiff(normalizedPrimaryScreen, normalizedSecondaryScreen));
		}
		else
		{
			builder.AppendLine("@@ screenText @@");
			builder.AppendLine("  no differences after normalization");
		}

		return builder.ToString().TrimEnd();
	}

	private static string BuildScreenDiff(string primaryScreenText, string secondaryScreenText)
	{
		string[] primaryLines = primaryScreenText.Split('\n');
		string[] secondaryLines = secondaryScreenText.Split('\n');
		int maxLineCount = Math.Max(primaryLines.Length, secondaryLines.Length);
		int emittedDifferences = 0;
		const int maxDifferencesToShow = 20;
		StringBuilder builder = new();

		builder.AppendLine("@@ screenText @@");
		for (int index = 0; index < maxLineCount; index++)
		{
			string primaryLine = index < primaryLines.Length ? primaryLines[index] : "";
			string secondaryLine = index < secondaryLines.Length ? secondaryLines[index] : "";
			if (string.Equals(primaryLine, secondaryLine, StringComparison.Ordinal))
			{
				continue;
			}

			emittedDifferences++;
			builder.AppendLine($"  line {index + 1}");
			builder.AppendLine($"- {FormatDiffLine(primaryLine)}");
			builder.AppendLine($"+ {FormatDiffLine(secondaryLine)}");
			if (emittedDifferences >= maxDifferencesToShow)
			{
				if (maxLineCount > index + 1)
				{
					builder.AppendLine("  ... additional differing lines omitted ...");
				}

				break;
			}
		}

		return builder.ToString();
	}

	private static string FormatDiffLine(string value) =>
		value.Length == 0
			? "<empty>"
			: value.Replace("\t", "\\t", StringComparison.Ordinal).Replace("\r", "\\r", StringComparison.Ordinal);

	private static string NormalizeScreenText(string text) =>
		string.IsNullOrEmpty(text)
			? string.Empty
			: text.Replace('─', '-');

	private static string DescribeProcessFailure(string toolName, ProcessResult process) =>
		$"{toolName} exited with code {process.ExitCode}.{Environment.NewLine}stdout:{Environment.NewLine}{process.StandardOutput}{Environment.NewLine}stderr:{Environment.NewLine}{process.StandardError}";

	private static string ToRepoRelativePath(string path)
	{
		string fullPath = Path.GetFullPath(path);
		string relativePath = Path.GetRelativePath(RepositoryLayout.Root, fullPath).Replace('\\', '/');
		return relativePath;
	}

	private static string? ResolveOptionalRepoPath(string? path) =>
		string.IsNullOrWhiteSpace(path)
			? null
			: Path.Combine(RepositoryLayout.Root, path.Replace('/', Path.DirectorySeparatorChar));

	private static void PrintUsage()
	{
		Console.Error.WriteLine("Usage: Cornerstone.TestCaseTool --name <case-name> [--shipped | --mme <path> | --source <cas-path>] [--obj <path>] [--data-dir <path>] [--input-text <text> | --input-file <path>] [--tested-in linchpin|linchpinst] [--limit <count>] [--update]");
	}

	private sealed record Options(
		string Name,
		bool UseShipped,
		string? MmePath,
		string? ObjPath,
		string? SourcePath,
		string? DataDirectoryPath,
		string InputText,
		InterpreterKind TestedInterpreter,
		int? InstructionLimit,
		bool UpdateExisting)
	{
		public static Options Parse(string[] args)
		{
			string? name = null;
			bool useShipped = false;
			string? mmePath = null;
			string? objPath = null;
			string? sourcePath = null;
			string? dataDirectoryPath = null;
			string? inputText = null;
			InterpreterKind testedInterpreter = InterpreterKind.Linchpin;
			int? instructionLimit = null;
			bool updateExisting = false;

			for (int index = 0; index < args.Length; index++)
			{
				string argument = args[index];
				if (argument.Equals("--name", StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length)
				{
					name = args[++index];
				}
				else if (argument.Equals("--shipped", StringComparison.OrdinalIgnoreCase))
				{
					useShipped = true;
				}
				else if (argument.Equals("--mme", StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length)
				{
					mmePath = args[++index];
				}
				else if (argument.Equals("--obj", StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length)
				{
					objPath = args[++index];
				}
				else if (argument.Equals("--source", StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length)
				{
					sourcePath = args[++index];
				}
				else if (argument.Equals("--data-dir", StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length)
				{
					dataDirectoryPath = args[++index];
				}
				else if (argument.Equals("--input-text", StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length)
				{
					if (inputText is not null)
					{
						throw new ToolException("Use either --input-text or --input-file, not both.");
					}

					inputText = args[++index];
				}
				else if (argument.Equals("--input-file", StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length)
				{
					if (inputText is not null)
					{
						throw new ToolException("Use either --input-text or --input-file, not both.");
					}

					string inputPath = Path.GetFullPath(args[++index]);
					if (!File.Exists(inputPath))
					{
						throw new ToolException($"Input file '{inputPath}' does not exist.");
					}

					inputText = Encoding.Latin1.GetString(File.ReadAllBytes(inputPath));
				}
				else if (argument.Equals("--tested-in", StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length)
				{
					testedInterpreter = ParseInterpreter(args[++index]);
				}
				else if (argument.Equals("--limit", StringComparison.OrdinalIgnoreCase) && index + 1 < args.Length)
				{
					if (!int.TryParse(args[++index], out int parsedLimit) || parsedLimit <= 0)
					{
						throw new ToolException("--limit must be a positive integer.");
					}

					instructionLimit = parsedLimit;
				}
				else if (argument.Equals("--update", StringComparison.OrdinalIgnoreCase))
				{
					updateExisting = true;
				}
				else
				{
					throw new ToolException($"Unknown or incomplete argument '{argument}'.");
				}
			}

			if (string.IsNullOrWhiteSpace(name))
			{
				throw new ToolException("--name must be supplied.");
			}

			if (useShipped && !string.IsNullOrWhiteSpace(mmePath))
			{
				throw new ToolException("Use either --shipped or --mme, not both.");
			}

			if (useShipped && !string.IsNullOrWhiteSpace(sourcePath))
			{
				throw new ToolException("Use either --shipped or --source, not both.");
			}

			if (!useShipped && string.IsNullOrWhiteSpace(mmePath) && string.IsNullOrWhiteSpace(sourcePath))
			{
				throw new ToolException("Supply one of --shipped, --mme, or --source.");
			}

			string? fullMmePath = null;
			if (useShipped)
			{
				fullMmePath = RepositoryLayout.ShippedMmePath;
			}
			else if (!string.IsNullOrWhiteSpace(mmePath))
			{
				fullMmePath = Path.GetFullPath(mmePath);
			}

			if (!string.IsNullOrWhiteSpace(fullMmePath) && !File.Exists(fullMmePath))
			{
				throw new ToolException($"MME file '{fullMmePath}' does not exist.");
			}

			string? fullObjPath = null;
			if (!string.IsNullOrWhiteSpace(objPath))
			{
				if (useShipped)
				{
					throw new ToolException("--obj cannot be combined with --shipped.");
				}

				fullObjPath = Path.GetFullPath(objPath);
				if (!File.Exists(fullObjPath))
				{
					throw new ToolException($"OBJ file '{fullObjPath}' does not exist.");
				}
			}
			else if (useShipped)
			{
				fullObjPath = RepositoryLayout.ShippedObjPath;
			}
			else if (!string.IsNullOrWhiteSpace(fullMmePath))
			{
				fullObjPath = Path.ChangeExtension(fullMmePath, ".obj");
				if (!File.Exists(fullObjPath))
				{
					throw new ToolException($"OBJ file '{fullObjPath}' does not exist.");
				}
			}

			if (!string.IsNullOrWhiteSpace(sourcePath))
			{
				sourcePath = Path.GetFullPath(sourcePath);
				if (!File.Exists(sourcePath))
				{
					throw new ToolException($"Source file '{sourcePath}' does not exist.");
				}
			}

			if (!string.IsNullOrWhiteSpace(dataDirectoryPath))
			{
				dataDirectoryPath = Path.GetFullPath(dataDirectoryPath);
				if (!Directory.Exists(dataDirectoryPath))
				{
					throw new ToolException($"Data directory '{dataDirectoryPath}' does not exist.");
				}
			}
			else if (useShipped)
			{
				dataDirectoryPath = RepositoryLayout.ShippedDataDirectoryPath;
			}

			return new Options(name, useShipped, fullMmePath, fullObjPath, sourcePath, dataDirectoryPath, inputText ?? string.Empty, testedInterpreter, instructionLimit, updateExisting);
		}

		private static InterpreterKind ParseInterpreter(string value) => value.ToLowerInvariant() switch
		{
			"linchpin" => InterpreterKind.Linchpin,
			"linchpinst" => InterpreterKind.LinchpinSt,
			_ => throw new ToolException("--tested-in must be either 'linchpin' or 'linchpinst'."),
		};
	}
}

internal enum InterpreterKind
{
	Linchpin,
	LinchpinSt,
}

internal sealed record RuntimeExecutionRequest(
	string Name,
	string? MmePath,
	string? ObjPath,
	string? SourcePath,
	string? DataDirectoryPath,
	string InputText,
	int? InstructionLimit);

internal sealed record ToolRuntimeReport(int HaltCode, int ExecutedInstructionCount, string ScreenText);

internal sealed record ProcessResult(int ExitCode, string StandardOutput, string StandardError);

internal sealed record ToolRunOutcome(ToolRuntimeReport Report, ProcessResult Process);

internal sealed record RuntimeImagePaths(string MmePath, string ObjPath);

internal sealed record ManifestCase(
	string Name,
	string? MmePath,
	string? ObjPath,
	string? ChiselSourcePath,
	string? DataDirectoryPath,
	string InputText,
	int ExpectedInstructionCount,
	int? InstructionLimit,
	string ExpectedScreenPath);


internal static class ProgramJson
{
	public static JsonSerializerOptions Options { get; } = new()
	{
		PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
		WriteIndented = true,
	};
}

internal sealed class ToolException : Exception
{
	public ToolException(string message)
		: base(message)
	{
	}

	public ToolException(string message, Exception innerException)
		: base(message, innerException)
	{
	}
}