/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Text;

namespace Linchpin;

/// <summary>
/// Controls how the VM's screen output is presented: as a text transcript or on a live console.
/// </summary>
internal enum VmRunDisplayMode
{
	Transcript,
	LiveConsole,
}

/// <summary>
/// The reason execution stopped.
/// </summary>
internal enum VmExecutionStopReason
{
	Halt,
	InstructionLimit,
	OutputPattern,
	KbInputCount,
}

/// <summary>
/// Identifies a procedure by its module and start offset, used as a target for tracing.
/// </summary>
internal readonly record struct VmProcedureTarget(int ModuleId, int StartOffset);

/// <summary>
/// The result of a VM execution run: halt code, instruction count, screen text, and stop details.
/// </summary>
internal sealed record VmExecutionResult(
	ushort HaltCode,
	int ExecutedInstructionCount,
	string ScreenText,
	bool StoppedByInstructionLimit,
	VmExecutionStopReason StopReason,
	string? StopDetail);

/// <summary>
/// How much VM state to include alongside each traced instruction.
/// </summary>
internal enum VmInstructionTraceStateMode
{
	None,
	Frame,
	Full,
}

/// <summary>
/// Trace configuration loaded from environment variables. Controls which procedures and
/// instructions are traced, output file path, and various limits.
/// </summary>
internal sealed record VmTraceConfiguration(
	IReadOnlyList<VmProcedureTarget> ProcedureTraceTargets,
	bool InstructionTraceEnabled,
	IReadOnlyList<VmProcedureTarget> InstructionTraceTargets,
	VmInstructionTraceStateMode InstructionStateMode,
	string? TraceFilePath,
	int TraceMaxLines,
	long TraceMaxBytes,
	int LocalsLimit,
	int StackLimit,
	IReadOnlyList<string> SymbolPathHints,
	string? StopOnOutputPattern,
	int? StopAfterKbInputCount,
	int? ModuleFilter,
	int? ProcedureFilter,
	int? PcStart,
	int? PcEnd,
	int? InstructionCountStart)
{
	/// <summary>
	/// Reads all <c>LINCHPIN_TRACE_*</c> and <c>LINCHPIN_STOP_*</c> environment variables
	/// and builds a trace configuration.
	/// </summary>
	public static VmTraceConfiguration Load()
	{
		IReadOnlyList<VmProcedureTarget> procedureTraceTargets = ParseProcedureTargets("LINCHPIN_TRACE_PROCS");
		IReadOnlyList<VmProcedureTarget> instructionTraceTargets = ParseProcedureTargets("LINCHPIN_TRACE_INSNS");
		VmInstructionTraceStateMode instructionStateMode = ParseStateMode(Environment.GetEnvironmentVariable("LINCHPIN_TRACE_STATE"));
		bool instructionTraceEnabled = IsEnabled("LINCHPIN_TRACE_INSTRUCTIONS")
			|| instructionStateMode != VmInstructionTraceStateMode.None
			|| instructionTraceTargets.Count > 0;
		if (instructionTraceEnabled && instructionStateMode == VmInstructionTraceStateMode.None)
		{
			instructionStateMode = VmInstructionTraceStateMode.Frame;
		}

		return new VmTraceConfiguration(
			procedureTraceTargets,
			instructionTraceEnabled,
			instructionTraceTargets,
			instructionStateMode,
			ResolveTraceFilePath(),
			ParseNonNegativeInt("LINCHPIN_TRACE_MAX_LINES", 50_000),
			ParseNonNegativeLong("LINCHPIN_TRACE_MAX_BYTES", 16L * 1024 * 1024),
			ParsePositiveInt("LINCHPIN_TRACE_LOCALS_LIMIT", 8),
			ParsePositiveInt("LINCHPIN_TRACE_STACK_LIMIT", 8),
			ParsePathList("LINCHPIN_SYMBOL_PATHS"),
			ParseOptionalString("LINCHPIN_STOP_ON_OUTPUT"),
			ParseOptionalPositiveInt("LINCHPIN_STOP_AFTER_KBINPUT"),
			ParseOptionalInt("LINCHPIN_TRACE_MODULE"),
			ParseOptionalInt("LINCHPIN_TRACE_PROC"),
			ParseOptionalInt("LINCHPIN_TRACE_PC_START"),
			ParseOptionalInt("LINCHPIN_TRACE_PC_END"),
			ParseOptionalInt("LINCHPIN_TRACE_STEP_START"));
	}

	public bool HasConditionalStop => !string.IsNullOrWhiteSpace(StopOnOutputPattern) || StopAfterKbInputCount.HasValue;

	public bool MatchesProcedure(int moduleId, int procedureStartOffset)
	{
		if (ProcedureTraceTargets.Count == 0)
		{
			return false;
		}

		return ProcedureTraceTargets.Contains(new VmProcedureTarget(moduleId, procedureStartOffset));
	}

	public bool MatchesInstructionProcedure(int moduleId, int procedureStartOffset)
	{
		if (InstructionTraceTargets.Count == 0)
		{
			return false;
		}

		return InstructionTraceTargets.Contains(new VmProcedureTarget(moduleId, procedureStartOffset));
	}

	public bool MatchesInstruction(int moduleId, int procedureIndex, int programCounter)
	{
		if (ModuleFilter.HasValue && moduleId != ModuleFilter.Value)
		{
			return false;
		}

		if (ProcedureFilter.HasValue && procedureIndex != ProcedureFilter.Value)
		{
			return false;
		}

		if (PcStart.HasValue && programCounter < PcStart.Value)
		{
			return false;
		}

		if (PcEnd.HasValue && programCounter > PcEnd.Value)
		{
			return false;
		}

		return true;
	}

	private static string? ResolveTraceFilePath()
	{
		string? traceFilePath = Environment.GetEnvironmentVariable("LINCHPIN_TRACE_FILE");
		return string.IsNullOrWhiteSpace(traceFilePath)
			? null
			: Path.GetFullPath(traceFilePath);
	}

	private static VmInstructionTraceStateMode ParseStateMode(string? stateMode)
	{
		if (string.IsNullOrWhiteSpace(stateMode))
		{
			return VmInstructionTraceStateMode.None;
		}

		return stateMode.Trim().ToLowerInvariant() switch
		{
			"none" => VmInstructionTraceStateMode.None,
			"frame" => VmInstructionTraceStateMode.Frame,
			"full" => VmInstructionTraceStateMode.Full,
			_ => throw new LinchpinException($"LINCHPIN_TRACE_STATE must be one of: none, frame, full. Found '{stateMode}'."),
		};
	}

	private static bool IsEnabled(string variableName)
	{
		string? value = Environment.GetEnvironmentVariable(variableName);
		return string.Equals(value, "1", StringComparison.Ordinal)
			|| string.Equals(value, "true", StringComparison.OrdinalIgnoreCase);
	}

	private static int ParsePositiveInt(string variableName, int defaultValue)
	{
		string? raw = Environment.GetEnvironmentVariable(variableName);
		if (string.IsNullOrWhiteSpace(raw))
		{
			return defaultValue;
		}

		int parsed = ParseInteger(variableName, raw);
		if (parsed <= 0)
		{
			throw new LinchpinException($"{variableName} must be a positive integer. Found '{raw}'.");
		}

		return parsed;
	}

	private static int ParseNonNegativeInt(string variableName, int defaultValue)
	{
		string? raw = Environment.GetEnvironmentVariable(variableName);
		if (string.IsNullOrWhiteSpace(raw))
		{
			return defaultValue;
		}

		int parsed = ParseInteger(variableName, raw);
		if (parsed < 0)
		{
			throw new LinchpinException($"{variableName} must be zero or a positive integer. Found '{raw}'.");
		}

		return parsed;
	}

	private static long ParseNonNegativeLong(string variableName, long defaultValue)
	{
		string? raw = Environment.GetEnvironmentVariable(variableName);
		if (string.IsNullOrWhiteSpace(raw))
		{
			return defaultValue;
		}

		long parsed = ParseLong(variableName, raw);
		if (parsed < 0)
		{
			throw new LinchpinException($"{variableName} must be zero or a positive integer. Found '{raw}'.");
		}

		return parsed;
	}

	private static int? ParseOptionalInt(string variableName)
	{
		string? raw = Environment.GetEnvironmentVariable(variableName);
		return string.IsNullOrWhiteSpace(raw)
			? null
			: ParseInteger(variableName, raw);
	}

	private static int? ParseOptionalPositiveInt(string variableName)
	{
		string? raw = Environment.GetEnvironmentVariable(variableName);
		if (string.IsNullOrWhiteSpace(raw))
		{
			return null;
		}

		int parsed = ParseInteger(variableName, raw);
		if (parsed <= 0)
		{
			throw new LinchpinException($"{variableName} must be a positive integer. Found '{raw}'.");
		}

		return parsed;
	}

	private static string? ParseOptionalString(string variableName)
	{
		string? raw = Environment.GetEnvironmentVariable(variableName);
		return string.IsNullOrWhiteSpace(raw)
			? null
			: raw;
	}

	private static IReadOnlyList<VmProcedureTarget> ParseProcedureTargets(string variableName)
	{
		string? raw = Environment.GetEnvironmentVariable(variableName);
		if (string.IsNullOrWhiteSpace(raw))
		{
			return Array.Empty<VmProcedureTarget>();
		}

		return raw
			.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
			.Select(token => ParseProcedureTarget(variableName, token))
			.Distinct()
			.ToArray();
	}

	private static VmProcedureTarget ParseProcedureTarget(string variableName, string token)
	{
		string[] parts = token.Split(':', 2, StringSplitOptions.TrimEntries);
		if (parts.Length != 2)
		{
			throw new LinchpinException($"{variableName} entries must look like module:startOffset. Found '{token}'.");
		}

		int moduleId = ParseInteger(variableName, parts[0]);
		int startOffset = ParseInteger(variableName, parts[1]);
		if (moduleId <= 0 || startOffset < 0)
		{
			throw new LinchpinException($"{variableName} entries must use a positive module id and non-negative start offset. Found '{token}'.");
		}

		return new VmProcedureTarget(moduleId, startOffset);
	}

	private static IReadOnlyList<string> ParsePathList(string variableName)
	{
		string? raw = Environment.GetEnvironmentVariable(variableName);
		if (string.IsNullOrWhiteSpace(raw))
		{
			return Array.Empty<string>();
		}

		return raw
			.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
			.Select(Path.GetFullPath)
			.Distinct(StringComparer.OrdinalIgnoreCase)
			.ToArray();
	}

	private static int ParseInteger(string variableName, string rawValue)
	{
		try
		{
			return rawValue.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
				? int.Parse(rawValue[2..], System.Globalization.NumberStyles.HexNumber, System.Globalization.CultureInfo.InvariantCulture)
				: int.Parse(rawValue, System.Globalization.CultureInfo.InvariantCulture);
		}
		catch (FormatException)
		{
			throw new LinchpinException($"{variableName} must be an integer value. Found '{rawValue}'.");
		}
	}

	private static long ParseLong(string variableName, string rawValue)
	{
		try
		{
			return rawValue.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
				? long.Parse(rawValue[2..], System.Globalization.NumberStyles.HexNumber, System.Globalization.CultureInfo.InvariantCulture)
				: long.Parse(rawValue, System.Globalization.CultureInfo.InvariantCulture);
		}
		catch (FormatException)
		{
			throw new LinchpinException($"{variableName} must be an integer value. Found '{rawValue}'.");
		}
	}
}

/// <summary>
/// Manages a thread-safe, line-and-byte-limited trace output stream, writing to the configured
/// trace file or to stderr if no file is specified.
/// </summary>
internal static class VmTraceSession
{
	private static readonly object SyncRoot = new();
	private static readonly VmTraceConfiguration configuration = VmTraceConfiguration.Load();
	private static readonly Encoding TraceEncoding = new UTF8Encoding(false);
	private static TextWriter? writer;
	private static int lineCount;
	private static long byteCount;
	private static bool truncationNotified;

	public static VmTraceConfiguration Configuration => configuration;

	public static bool IsEnabled => configuration.ProcedureTraceTargets.Count > 0
		|| configuration.InstructionTraceEnabled;

	public static void Write(string message)
	{
		if (!IsEnabled)
		{
			return;
		}

		lock (SyncRoot)
		{
			if (ShouldSuppressFurtherOutput())
			{
				EmitTruncationNotice();
				return;
			}

			TextWriter output = EnsureWriter();
			string line = $"[linchpin] {message}";
			output.WriteLine(line);
			output.Flush();
			lineCount++;
			byteCount += TraceEncoding.GetByteCount(line) + TraceEncoding.GetByteCount(Environment.NewLine);
		}
	}

	private static bool ShouldSuppressFurtherOutput()
	{
		return (configuration.TraceMaxLines > 0 && lineCount >= configuration.TraceMaxLines)
			|| (configuration.TraceMaxBytes > 0 && byteCount >= configuration.TraceMaxBytes);
	}

	private static void EmitTruncationNotice()
	{
		if (truncationNotified)
		{
			return;
		}

		truncationNotified = true;
		TextWriter output = EnsureWriter();
		string notice = BuildTruncationNotice();
		output.WriteLine(notice);
		output.Flush();
		lineCount++;
		byteCount += TraceEncoding.GetByteCount(notice) + TraceEncoding.GetByteCount(Environment.NewLine);
	}

	private static string BuildTruncationNotice()
	{
		List<string> limits = new();
		if (configuration.TraceMaxLines > 0)
		{
			limits.Add($"lines={configuration.TraceMaxLines:N0}");
		}

		if (configuration.TraceMaxBytes > 0)
		{
			limits.Add($"bytes={configuration.TraceMaxBytes:N0}");
		}

		string limitText = limits.Count == 0 ? "configured limit" : string.Join(", ", limits);
		return $"[linchpin] trace truncated after reaching {limitText}; increase LINCHPIN_TRACE_MAX_LINES and/or LINCHPIN_TRACE_MAX_BYTES to capture more.";
	}

	private static TextWriter EnsureWriter()
	{
		if (writer is not null)
		{
			return writer;
		}

		if (string.IsNullOrWhiteSpace(configuration.TraceFilePath))
		{
			writer = Console.Error;
			return writer;
		}

		try
		{
			string traceFilePath = configuration.TraceFilePath;
			string? directory = Path.GetDirectoryName(traceFilePath);
			if (!string.IsNullOrWhiteSpace(directory))
			{
				Directory.CreateDirectory(directory);
			}

			writer = new StreamWriter(File.Open(traceFilePath, FileMode.Create, FileAccess.Write, FileShare.ReadWrite), TraceEncoding)
			{
				AutoFlush = true,
			};
			return writer;
		}
		catch (Exception exception)
		{
			throw new LinchpinException($"Could not open trace file '{configuration.TraceFilePath}': {exception.Message}");
		}
	}
}

/// <summary>
/// Runtime options for VM execution: file I/O permissions, snapshot behavior, and data directory path.
/// </summary>
internal readonly record struct VmExecutionOptions(
	bool HostWriteFiles = false,
	bool AllowInstructionLimitSnapshot = false,
	string? DataDirectoryPath = null);

