/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Text;

namespace Linchpin;

internal sealed partial class VmRuntimeState
{

	/// <summary>
	/// Emits a detailed instruction-level trace line if the current procedure and PC match
	/// the active trace configuration. Includes opcode, locals, stack state, and optionally
	/// full frame and arena details.
	/// </summary>
	public void TraceInstructionExecution(DecodedInstruction instruction, int instructionCount)
	{
		VmTraceConfiguration configuration = VmTraceSession.Configuration;
		bool matchesStartOffset = TraceProcedureStartOffset is int tracedStartOffset && CurrentFrame.ProcedureStartOffset == tracedStartOffset;
		bool matchesProcedureTarget = configuration.MatchesInstructionProcedure(CurrentModuleId, CurrentFrame.ProcedureStartOffset);
		bool matchesLegacyFilter = configuration.InstructionTraceEnabled
			&& configuration.InstructionTraceTargets.Count == 0
			&& configuration.MatchesInstruction(CurrentModuleId, CurrentFrame.ProcedureIndex, ProgramCounter);
		if (!matchesStartOffset && (!matchesProcedureTarget && !matchesLegacyFilter
			|| (configuration.InstructionCountStart.HasValue && instructionCount < configuration.InstructionCountStart.Value)))
		{
			return;
		}

		string operands = instruction.Operands.Count == 0
			? string.Empty
			: " " + string.Join(", ", instruction.Operands.Select(operand => operand.DisplayText));
		int localsLimit = configuration.InstructionStateMode == VmInstructionTraceStateMode.Full
			? CurrentFrame.Locals.Length
			: configuration.LocalsLimit;
		int stackLimit = configuration.InstructionStateMode == VmInstructionTraceStateMode.Full
			? evaluationStack.Count
			: configuration.StackLimit;

		StringBuilder builder = new();
		builder.Append("STEP #")
			.Append(instructionCount.ToString("D7", System.Globalization.CultureInfo.InvariantCulture))
			.Append(" module=")
			.Append(CurrentModuleId)
			.Append(" proc=")
			.Append(ResolveProcedureName(CurrentModuleId, CurrentFrame.ProcedureIndex, CurrentFrame.ProcedureStartOffset))
			.Append(" start=0x")
			.Append(CurrentFrame.ProcedureStartOffset.ToString("X4", System.Globalization.CultureInfo.InvariantCulture))
			.Append(" pc=0x")
			.Append(ProgramCounter.ToString("X4", System.Globalization.CultureInfo.InvariantCulture))
			.Append(" opcode=")
			.Append(instruction.Mnemonic)
			.Append(operands)
			.Append(" locals=[")
			.Append(BuildLocalsPreview(localsLimit))
			.Append("] stack=[")
			.Append(BuildEvaluationStackPreview(stackLimit))
			.Append("] callDepth=")
			.Append(callStack.Count);

		if (configuration.InstructionStateMode == VmInstructionTraceStateMode.Full)
		{
			builder.Append(" frameStart=0x")
				.Append(CurrentFrame.ProcedureStartOffset.ToString("X4", System.Globalization.CultureInfo.InvariantCulture))
				.Append(" code=0x")
				.Append(CurrentFrame.CodeOffset.ToString("X4", System.Globalization.CultureInfo.InvariantCulture))
				.Append(" upper=0x")
				.Append(FrameUpperBound.ToString("X4", System.Globalization.CultureInfo.InvariantCulture))
				.Append(" tupleTop=0x")
				.Append(tupleStackByteOffset.ToString("X5", System.Globalization.CultureInfo.InvariantCulture))
				.Append(" arenas=[low=0x")
				.Append(nextLowArenaByteOffset.ToString("X5", System.Globalization.CultureInfo.InvariantCulture))
				.Append(", high=0x")
				.Append(nextHighArenaByteOffset.ToString("X5", System.Globalization.CultureInfo.InvariantCulture))
				.Append("] channels=[")
				.Append(BuildOpenChannelPreview())
				.Append("] calls=[")
				.Append(BuildCallStackPreview())
				.Append(']');
		}

		VmTraceSession.Write(builder.ToString());
	}

	/// <summary>
	/// Emits a procedure-entry trace line if the target procedure is being traced.
	/// </summary>
	private void TraceProcedureEntry(int moduleId, ProcedureEntry procedure, IReadOnlyList<ushort> locals, int argumentCount)
	{
		if (!VmTraceSession.Configuration.MatchesProcedure(moduleId, procedure.StartOffset))
		{
			return;
		}

		VmTraceSession.Write(
			$"ENTER {DescribeProcedure(moduleId, procedure.ProcedureIndex, procedure.StartOffset, procedure.CodeOffset)} "
			+ $"caller={DescribeCaller(CurrentModuleId, CurrentFrame, ProgramCounter)} argc={argumentCount} "
			+ $"locals=[{BuildLocalsPreview(locals, VmTraceSession.Configuration.LocalsLimit)}]");
	}

	/// <summary>
	/// Emits a procedure-exit trace line with the return values and caller context.
	/// </summary>
	private void TraceProcedureExit(int moduleId, VmFrame frame, IReadOnlyList<ushort> resultWords, CallContinuation? caller)
	{
		if (!VmTraceSession.Configuration.MatchesProcedure(moduleId, frame.ProcedureStartOffset))
		{
			return;
		}

		string resultPreview = resultWords.Count == 0
			? "<void>"
			: string.Join(", ", resultWords.Select((value, index) => $"R{index}=0x{value:X4}"));
		VmTraceSession.Write(
			$"EXIT {DescribeProcedure(moduleId, frame.ProcedureIndex, frame.ProcedureStartOffset, frame.CodeOffset)} "
			+ $"return=[{resultPreview}] caller={DescribeCaller(caller)}");
	}

	private static int? ParseTraceProcedureStartOffset()
	{
		string? value = Environment.GetEnvironmentVariable("LINCHPIN_TRACE_START_OFFSET");
		if (string.IsNullOrWhiteSpace(value))
		{
			return null;
		}

		if (value.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
		{
			value = value[2..];
		}

		return int.TryParse(value, System.Globalization.NumberStyles.HexNumber, System.Globalization.CultureInfo.InvariantCulture, out int parsed)
			? parsed
			: null;
	}

	private string DescribeCurrentExecutionContext()
	{
		string localsPreview = string.Join(", ", CurrentFrame.Locals.Take(Math.Min(4, CurrentFrame.Locals.Length)).Select((value, index) => $"L{index}=0x{value:X4}"));
		return $"module={CurrentModuleId}, pc=0x{ProgramCounter:X4}, proc={CurrentFrame.ProcedureIndex}, [{localsPreview}]";
	}

	/// <summary>
	/// Returns a human-readable summary of the current execution position for error messages.
	/// </summary>
	public string BuildExecutionSummary()
	{
		return $"module {CurrentModuleId} proc {CurrentFrame.ProcedureIndex} pc=0x{ProgramCounter:X4} locals=[{BuildLocalsPreview(8)}] stack=[{BuildEvaluationStackPreview(8)}] callDepth={callStack.Count + 1} callers=[{BuildCallStackPreview()}]";
	}

	private string BuildLocalsPreview(int limit)
	{
		if (CurrentFrame.Locals.Length == 0)
		{
			return "<none>";
		}

		int count = Math.Min(Math.Max(limit, 1), CurrentFrame.Locals.Length);
		string preview = string.Join(", ", CurrentFrame.Locals.Take(count).Select((value, index) => $"L{index}=0x{value:X4}"));
		return count < CurrentFrame.Locals.Length
			? $"{preview}, ..."
			: preview;
	}

	private string BuildEvaluationStackPreview(int limit)
	{
		if (evaluationStack.Count == 0)
		{
			return "<empty>";
		}

		int count = Math.Min(Math.Max(limit, 1), evaluationStack.Count);
		string preview = string.Join(", ", evaluationStack.Take(count).Select((value, index) => $"S{index}=0x{value:X4}"));
		return count < evaluationStack.Count
			? $"{preview}, ..."
			: preview;
	}

	private string BuildOpenChannelPreview()
	{
		if (openChannels.Count == 0)
		{
			return "<none>";
		}

		return string.Join(", ", openChannels.OrderBy(pair => pair.Key).Select(pair =>
		{
			VmChannel channel = pair.Value;
			string origin = channel.Path is null ? "mem" : "host";
			return $"{pair.Key}:{origin}:{SanitizeHostFileName(channel.Name)}@{channel.Position}/0x{channel.Contents.Length:X}";
		}));
	}

	private string BuildCallStackPreview()
	{
		if (callStack.Count == 0)
		{
			return "<root>";
		}

		return string.Join(" | ", callStack.Select(continuation =>
			$"m{continuation.ModuleId}:{ResolveProcedureName(continuation.ModuleId, continuation.Frame.ProcedureIndex, continuation.Frame.ProcedureStartOffset)}:return=0x{continuation.ReturnProgramCounter:X4}"));
	}

	private string BuildLocalsPreview(IReadOnlyList<ushort> locals, int limit)
	{
		if (locals.Count == 0)
		{
			return "<none>";
		}

		int count = Math.Min(Math.Max(limit, 1), locals.Count);
		string preview = string.Join(", ", locals.Take(count).Select((value, index) => $"L{index}=0x{value:X4}"));
		return count < locals.Count
			? $"{preview}, ..."
			: preview;
	}

	private string DescribeProcedure(int moduleId, int procedureIndex, int startOffset, int codeOffset)
	{
		string name = ResolveProcedureName(moduleId, procedureIndex, startOffset);
		return $"{name} (m{moduleId} start=0x{startOffset:X4} code=0x{codeOffset:X4})";
	}

	private string ResolveProcedureName(int moduleId, int procedureIndex, int startOffset)
	{
		if (procedureSymbols.TryGetName(moduleId, startOffset, out string? name) && !string.IsNullOrWhiteSpace(name))
		{
			return name;
		}

		return procedureIndex >= 0
			? $"Proc{procedureIndex:D3}"
			: $"ProcPriv_{startOffset:X4}";
	}

	private string DescribeCaller(int moduleId, VmFrame frame, int programCounter)
	{
		return $"{ResolveProcedureName(moduleId, frame.ProcedureIndex, frame.ProcedureStartOffset)}/pc=0x{programCounter:X4}";
	}

	private string DescribeCaller(CallContinuation? caller)
	{
		if (caller is null)
		{
			return "<root>";
		}

		return $"{ResolveProcedureName(caller.ModuleId, caller.Frame.ProcedureIndex, caller.Frame.ProcedureStartOffset)}/pc=0x{caller.ReturnProgramCounter:X4}";
	}

	private string DescribeLocalVectorPreview()
	{
		if (CurrentFrame.Locals.Length < 3)
		{
			return string.Empty;
		}

		ushort handle = CurrentFrame.Locals[2];
		if (handle == FalseSentinel)
		{
			return string.Empty;
		}

		try
		{
			string preview = string.Join(", ", Enumerable.Range(0, 8).Select(index => $"[{index}]=0x{ReadAggregateWord(handle, index):X4}"));
			return $", L2* handle=0x{handle:X4} {{{preview}}}";
		}
		catch (LinchpinException)
		{
			return string.Empty;
		}
	}

	private string BuildAggregatePreview(ushort handle, int wordCount)
	{
		if (handle == FalseSentinel)
		{
			return string.Empty;
		}

		try
		{
			string preview = string.Join(", ", Enumerable.Range(0, wordCount).Select(index => $"[{index}]=0x{ReadAggregateWord(handle, index):X4}"));
			return $" preview={{{preview}}}";
		}
		catch (LinchpinException)
		{
			return string.Empty;
		}
	}

	private static void Trace(string message)
	{
		if (VmTraceSession.Configuration.ProcedureTraceTargets.Count == 0)
		{
			return;
		}

		VmTraceSession.Write(message);
	}
}
