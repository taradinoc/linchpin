/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Linchpin;

internal sealed partial class VmRuntimeState
{
	private const ushort FalseSentinel = 0x8001;
	private const int LowSegmentByteLength = 0x10000;
	private const int HighSegmentByteLength = 0x10000;
	private const int TupleStackReserveBytes = HighSegmentByteLength / 64;
	private readonly Stack<ushort> evaluationStack = new();
	private readonly Dictionary<int, ushort> systemModuleGlobals = new();
	private readonly Stack<CallContinuation> callStack = new();
	private readonly Dictionary<string, byte[]> syntheticFiles = new(StringComparer.OrdinalIgnoreCase);
	private readonly HashSet<string> deletedSyntheticFiles = new(StringComparer.OrdinalIgnoreCase);
	private readonly Dictionary<ushort, VmChannel> openChannels = new();
	private readonly Dictionary<ushort, JumpSnapshot> jumpSnapshots = new();
	private readonly CornerstoneImage image;
	private readonly VmExecutionOptions executionOptions;
	private readonly string repositoryRoot = RepositoryLocator.FindRepositoryRoot();
	private readonly VmProcedureSymbolCatalog procedureSymbols;
	private static readonly int? TraceProcedureStartOffset = ParseTraceProcedureStartOffset();
	private readonly Dictionary<int, ProcedureEntry>[] proceduresByCodeOffset;
	private readonly Dictionary<int, ProcedureEntry>[] proceduresByStartOffset;
	private readonly Dictionary<int, int>[] frameUpperBoundsByProcedureStart;
	private int nextLowArenaByteOffset;
	private int nextHighArenaByteOffset;
	private int tupleStackByteOffset;
	private int tupleStackFloorByteOffset;
	private readonly Dictionary<int, int> freeVectorBlocks = new();
	private readonly Dictionary<ushort, ushort> managedAggregateWordCounts = new();
	private ushort nextChannelId = 1;
	private ushort activeChannelId = FalseSentinel;
	private ushort nextJumpToken = 0xA000;
	private int kbInputEventCount;

	private sealed record VmChannel(string Name, string? Path, byte[] Contents, int Mode, int Position);

	private sealed record JumpSnapshot(int ModuleId, int LongJumpProgramCounter, int LongJumpReturnProgramCounter, int TupleStackByteOffset, VmFrame Frame, int FrameUpperBound, ushort[] EvaluationStack, CallContinuation[] CallStack);

	private sealed record BitFieldReadSpec(int Shift, ushort ValueMask, int WordIndex, int? LocalIndex);

	private sealed record BitFieldWriteSpec(int Shift, ushort ValueMask, ushort WordMask, int WordIndex, int? LocalIndex);

	private sealed record SingleBitSpec(int BitNumber, bool BitValue, int WordIndex, int? LocalIndex);


	private VmRuntimeState(
		CornerstoneImage image,
		VmExecutionOptions executionOptions,
		int currentModuleId,
		int programCounter,
		byte[] ramBytes,
		ushort[] programGlobals,
		IReadOnlyList<ushort[]> moduleGlobals,
		VmFrame currentFrame,
		int frameUpperBound,
		VirtualScreenHost host)
	{
		this.image = image;
		this.executionOptions = executionOptions;
		CurrentModuleId = currentModuleId;
		ProgramCounter = programCounter;
		RamBytes = ramBytes;
		ProgramGlobals = programGlobals;
		ModuleGlobals = moduleGlobals;
		CurrentFrame = currentFrame;
		FrameUpperBound = frameUpperBound;
		Host = host;
		procedureSymbols = VmProcedureSymbolCatalog.Load(image, repositoryRoot, VmTraceSession.Configuration.SymbolPathHints);
		proceduresByCodeOffset = BuildProcedureLookupTables(image, static procedure => procedure.CodeOffset);
		proceduresByStartOffset = BuildProcedureLookupTables(image, static procedure => procedure.StartOffset);
		frameUpperBoundsByProcedureStart = BuildFrameUpperBoundCaches(image);
		nextLowArenaByteOffset = (image.InitialRamBytes.Length + 1) & ~1;
		nextHighArenaByteOffset = LowSegmentByteLength;
		tupleStackFloorByteOffset = LowSegmentByteLength + HighSegmentByteLength - TupleStackReserveBytes;
		tupleStackByteOffset = LowSegmentByteLength + HighSegmentByteLength;
		LoadInitialSyntheticFiles();
		InitializeSystemSlots();

		// MG[0x12] in Module 3 starts as FALSE (0x8001) in the shipped MME.
		// When false, ProcPriv_37EB skips the CMD debug menu and enters normal
		// KBINPUT flow. Setting it to 0 (truthy) would show the debug CMD menu.
		// Leave it at default to pursue the real main menu path.
		// if (ModuleGlobals.Count >= 3 && ModuleGlobals[2].Length > 0x12)
		// 	ModuleGlobals[2][0x12] = 0;
	}

	public int CurrentModuleId { get; private set; }
	public int ProgramCounter { get; set; }
	public byte[] RamBytes { get; }
	public ushort[] ProgramGlobals { get; }
	public IReadOnlyList<ushort[]> ModuleGlobals { get; }
	public VmFrame CurrentFrame { get; private set; }
	public int FrameUpperBound { get; private set; }
	public VirtualScreenHost Host { get; }
	public bool HasInstructionTracing { get; } = TraceProcedureStartOffset.HasValue
		|| VmTraceSession.Configuration.InstructionTraceEnabled
		|| VmTraceSession.Configuration.InstructionTraceTargets.Count > 0;
	public bool IsHalted { get; private set; }
	public ushort HaltCode { get; private set; }
	public int LastReturnWordCount { get; private set; }
	public int KbInputEventCount => kbInputEventCount;


	/// <summary>
	/// Creates a new <see cref="VmRuntimeState"/> from the image's bootstrap state and the provided
	/// input text, display mode, and execution options.
	/// </summary>
	public static VmRuntimeState Create(CornerstoneImage image, string inputText, VmRunDisplayMode displayMode, VmExecutionOptions executionOptions)
	{
		VmState bootstrapState = VmBootstrapper.CreateInitialState(image);
		ModuleImage module = image.Modules[bootstrapState.CurrentModuleId - 1];
		int frameUpperBound = module.Procedures
			.Where(candidate => candidate.StartOffset > bootstrapState.CurrentFrame.ProcedureStartOffset)
			.Select(candidate => candidate.StartOffset)
			.DefaultIfEmpty(module.Length)
			.Min();

		return new VmRuntimeState(
			image,
			executionOptions,
			bootstrapState.CurrentModuleId,
			bootstrapState.ProgramCounter,
			bootstrapState.RamBytes.ToArray(),
			bootstrapState.ProgramGlobals.ToArray(),
			bootstrapState.ModuleGlobals.Select(values => values.ToArray()).ToArray(),
			bootstrapState.CurrentFrame with { Locals = bootstrapState.CurrentFrame.Locals.ToArray() },
			frameUpperBound,
			new VirtualScreenHost(inputText, displayMode));
	}

	/// <summary>
	/// Sets up a near (intra-module) procedure call by resolving the target offset to a procedure,
	/// then entering it.
	/// </summary>
	public void EnterNearCall(ushort targetOffset, int argumentCount, int returnProgramCounter)
	{
		EnterProcedure(CurrentModuleId, ResolveNearProcedure(targetOffset, CurrentModuleId), argumentCount, returnProgramCounter);
	}

	/// <summary>
	/// Sets up a far (cross-module) procedure call by decoding the module/procedure selector
	/// and entering the target procedure.
	/// </summary>
	public void EnterFarCall(ushort selector, int argumentCount, int returnProgramCounter)
	{
		int moduleId = selector >> 8;
		int procedureIndex = selector & 0xFF;
		if (moduleId < 1 || moduleId > image.Modules.Count)
		{
			throw new LinchpinException($"Far call targets invalid module {moduleId}.");
		}

		ModuleImage module = image.Modules[moduleId - 1];
		if (procedureIndex < 0 || procedureIndex >= module.Procedures.Count)
		{
			throw new LinchpinException($"Far call targets invalid procedure {procedureIndex} in module {moduleId}.");
		}

		EnterProcedure(moduleId, module.Procedures[procedureIndex], argumentCount, returnProgramCounter);
	}

	/// <summary>
	/// Returns from the current procedure, pushing result words onto the caller's evaluation stack.
	/// If the call stack is empty (i.e. returning from the entry procedure), the VM halts.
	/// </summary>
	public void Return(IReadOnlyList<ushort> resultWords)
	{
		LastReturnWordCount = resultWords.Count;
		CallContinuation? caller = callStack.Count == 0 ? null : callStack.Peek();
		TraceProcedureExit(CurrentModuleId, CurrentFrame, resultWords, caller);

		if (callStack.Count == 0)
		{
			Halt(resultWords.Count == 0 ? (ushort)0 : resultWords[0]);
			return;
		}

		CallContinuation continuation = callStack.Pop();
		CurrentModuleId = continuation.ModuleId;
		ProgramCounter = continuation.ReturnProgramCounter;
		CurrentFrame = continuation.Frame;
		FrameUpperBound = continuation.FrameUpperBound;
		RestoreEvaluationStackDepth(continuation.EvaluationStackDepth);

		for (int index = 0; index < resultWords.Count; index++)
		{
			Push(resultWords[index]);
		}
	}

	/// <summary>
	/// Discards the most recent return bundle from the evaluation stack, used after
	/// a PULLRET/POPRET when the caller doesn't need the return values.
	/// </summary>
	public void DiscardReturnBundle()
	{
		for (int index = 0; index < LastReturnWordCount; index++)
		{
			Pop();
		}
	}

	public void Push(ushort value) => evaluationStack.Push(value);

	public ushort Pop()
	{
		if (evaluationStack.Count == 0)
		{
			throw new LinchpinException($"VM stack underflow ({BuildExecutionSummary()}).");
		}

		return evaluationStack.Pop();
	}

	public ushort Peek()
	{
		if (evaluationStack.Count == 0)
		{
			throw new LinchpinException("VM stack underflow.");
		}

		return evaluationStack.Peek();
	}

	public void PushWordOperand(DecodedInstruction instruction, int operandIndex)
	{
		Push((ushort)instruction.Operands[operandIndex].RawValue);
	}

	public ushort LoadLocal(int index)
	{
		if (index < 0 || index >= CurrentFrame.Locals.Length)
		{
			throw new LinchpinException($"Local index {index} is out of range for the current frame.");
		}

		return CurrentFrame.Locals[index];
	}

	public void StoreLocal(int index, ushort value, bool preserveTop)
	{
		if (index < 0 || index >= CurrentFrame.Locals.Length)
		{
			throw new LinchpinException($"Local index {index} is out of range for the current frame.");
		}

		CurrentFrame.Locals[index] = value;
		if (!preserveTop)
		{
			return;
		}
	}

	/// <summary>
	/// Reads a module global variable. For indices beyond the module's declared globals,
	/// reads from system slots (clock, lock keys, abort flag, etc.).
	/// </summary>
	public ushort LoadModuleGlobal(int index)
	{
		ushort[] moduleGlobals = ModuleGlobals[CurrentModuleId - 1];
		if (index >= 0 && index < moduleGlobals.Length)
		{
			return moduleGlobals[index];
		}

		if (index is >= 0xCD and <= 0xD2)
		{
			return LoadClockSystemSlot(index);
		}

		if (index == 0xD6)
		{
			return Host.GetLockKeyState();
		}

		// 0xD4 is the user-break / abort-status flag.  Shipped code only ever
		// reads it (LOADMG) and gates on JUMPF, treating FALSE as "no break".
		// Default must be FALSE so the abort path is not entered spuriously.
		if (index == 0xD4)
		{
			return systemModuleGlobals.TryGetValue(index, out ushort breakFlag) ? breakFlag : FalseSentinel;
		}

		ushort sysValue = systemModuleGlobals.TryGetValue(index, out ushort v) ? v : (ushort)0;
		return sysValue;
	}

	private ushort LoadClockSystemSlot(int index)
	{
		(ushort month, ushort day, ushort year) = Host.GetCurrentDate();
		(ushort hour, ushort minute, ushort second) = Host.GetCurrentTime();
		return index switch
		{
			0xCD => month,
			0xCE => day,
			0xCF => year,
			0xD0 => hour,
			0xD1 => minute,
			0xD2 => second,
			_ => throw new LinchpinException($"System slot 0x{index:X2} is not a clock slot."),
		};
	}

	public bool IsInputSuppressed()
	{
		return systemModuleGlobals.TryGetValue(0xC0, out ushort v) && v != 0;
	}

	public ushort LoadProgramGlobal(int index)
	{
		if (index < 0 || index >= ProgramGlobals.Length)
		{
			throw new LinchpinException($"Program global index 0x{index:X2} is out of range.");
		}

		return ProgramGlobals[index];
	}

	public void StoreProgramGlobal(int index, ushort value)
	{
		if (index < 0 || index >= ProgramGlobals.Length)
		{
			throw new LinchpinException($"Program global index 0x{index:X2} is out of range.");
		}

		ProgramGlobals[index] = value;
	}

	/// <summary>
	/// Writes a module global variable. For indices beyond the module's declared globals,
	/// writes to system slots and notifies the display host of display-related changes.
	/// </summary>
	public void StoreModuleGlobal(int index, ushort value)
	{
		ushort[] moduleGlobals = ModuleGlobals[CurrentModuleId - 1];
		if (index >= 0 && index < moduleGlobals.Length)
		{
			moduleGlobals[index] = value;

			return;
		}

		systemModuleGlobals[index] = value;
		Host.NotifyModuleGlobalWrite(index, value);
	}

	/// <summary>
	/// Captures a SETJMP snapshot of the current evaluation stack, call stack, and tuple stack,
	/// returning a token that can later be used by LONGJMP to restore this state.
	/// </summary>
	public ushort CreateSetJumpToken(int protectedOffset, int landingOffset)
	{
		ushort token = nextJumpToken++;
		if (token == 0x8001)
		{
			token = nextJumpToken++;
		}

		jumpSnapshots[token] = new JumpSnapshot(
			CurrentModuleId,
			protectedOffset,
			landingOffset,
			tupleStackByteOffset,
			CurrentFrame,
			FrameUpperBound,
			evaluationStack.Reverse().ToArray(),
			callStack.Reverse().ToArray());

		return token;
	}

	/// <summary>
	/// Restores a SETJMP snapshot's stack and frame state, jumping to its protected (non-return) target.
	/// </summary>
	public void LongJump(ushort token)
	{
		RestoreJumpSnapshot(token, null, useReturnTarget: false);
	}

	/// <summary>
	/// Restores a SETJMP snapshot and pushes a return value, jumping to the snapshot's return target.
	/// </summary>
	public void LongJumpReturn(ushort token, ushort returnValue)
	{
		RestoreJumpSnapshot(token, returnValue, useReturnTarget: true);
	}

	public void Halt(ushort code)
	{
		IsHalted = true;
		HaltCode = code;
	}


	private void EnterProcedure(int moduleId, ProcedureEntry procedure, int argumentCount, int returnProgramCounter)
	{
		ushort[] arguments = PopArguments(argumentCount);
		int evaluationStackDepth = evaluationStack.Count;
		ushort[] locals = new ushort[procedure.Header.LocalCount];
		Array.Fill(locals, FalseSentinel);
		ApplyInitializers(procedure, locals);
		for (int index = 0; index < Math.Min(argumentCount, locals.Length); index++)
		{
			locals[index] = arguments[index];
		}

		callStack.Push(new CallContinuation(CurrentModuleId, returnProgramCounter, CurrentFrame, FrameUpperBound, evaluationStackDepth));
		TraceProcedureEntry(moduleId, procedure, locals, argumentCount);

		CurrentModuleId = moduleId;
		CurrentFrame = new VmFrame(moduleId, procedure.ProcedureIndex, procedure.StartOffset, procedure.CodeOffset, locals);
		ProgramCounter = procedure.CodeOffset;
		FrameUpperBound = ComputeFrameUpperBound(moduleId, procedure.StartOffset);
	}

	private string FormatAggregatePreview(ushort handle)
	{
		try
		{
			return string.Concat(Enumerable.Range(0, 8).Select(index => $"{ReadAggregateByte(handle, index):X2}"));
		}
		catch
		{
			return "?";
		}
	}

	private readonly record struct ExtractRecordSelection(int WordOffset, int WordLength, int PayloadByteOffset, int PayloadByteLength, byte Tag, byte ComponentCount);

	private readonly record struct ExtractSubfieldSelection(byte ByteLength, int DataByteOffset, int EncodedByteLength, int WordLength);

	public void RecordKbInputEvent(ushort value)
	{
		if (value != FalseSentinel)
		{
			kbInputEventCount++;
		}
	}

	private ProcedureEntry ResolveNearProcedure(ushort targetOffset, int moduleId)
	{
		ModuleImage module = image.Modules[moduleId - 1];
		Dictionary<int, ProcedureEntry> proceduresForModuleByCodeOffset = proceduresByCodeOffset[moduleId - 1];
		if (proceduresForModuleByCodeOffset.TryGetValue(targetOffset, out ProcedureEntry? procedure))
		{
			return procedure;
		}

		Dictionary<int, ProcedureEntry> proceduresForModuleByStartOffset = proceduresByStartOffset[moduleId - 1];
		if (proceduresForModuleByStartOffset.TryGetValue(targetOffset, out procedure))
		{
			return procedure;
		}

		if (procedure is null)
		{
			procedure = ParsePrivateProcedure(module, targetOffset);
			proceduresForModuleByStartOffset[targetOffset] = procedure;
		}

		return procedure;
	}

	private ProcedureEntry ParsePrivateProcedure(ModuleImage module, int startOffset)
	{
		int absoluteOffset = module.ObjectOffset + startOffset;
		int moduleEndOffset = module.ObjectOffset + module.Length;
		if (absoluteOffset >= moduleEndOffset)
		{
			throw new LinchpinException($"Near call target 0x{startOffset:X4} falls outside module {module.ModuleId}.");
		}

		int cursor = absoluteOffset;
		byte headerByte = image.ObjectBytes[cursor++];
		int localCount = headerByte & 0x7F;
		bool hasInitializers = (headerByte & 0x80) != 0;
		List<ProcedureInitializer> initializers = new();

		if (hasInitializers)
		{
			while (true)
			{
				if (cursor >= moduleEndOffset)
				{
					throw new LinchpinException($"Private procedure at 0x{startOffset:X4} has a truncated initializer list.");
				}

				byte marker = image.ObjectBytes[cursor++];
				int localIndex = marker & 0x3F;
				bool valueIsByte = (marker & 0x40) != 0;
				bool isLast = (marker & 0x80) != 0;
				ushort value;

				if (valueIsByte)
				{
					if (cursor >= moduleEndOffset)
					{
						throw new LinchpinException($"Private procedure at 0x{startOffset:X4} truncates a byte initializer.");
					}

					value = unchecked((ushort)(short)(sbyte)image.ObjectBytes[cursor++]);
				}
				else
				{
					if (cursor + 1 >= moduleEndOffset)
					{
						throw new LinchpinException($"Private procedure at 0x{startOffset:X4} truncates a word initializer.");
					}

					value = (ushort)(image.ObjectBytes[cursor] | (image.ObjectBytes[cursor + 1] << 8));
					cursor += 2;
				}

				initializers.Add(new ProcedureInitializer(localIndex, value, valueIsByte));
				if (isLast)
				{
					break;
				}
			}
		}

		return new ProcedureEntry(-1, startOffset, startOffset + (cursor - absoluteOffset), new ProcedureHeader(localCount, cursor - absoluteOffset, initializers));
	}

	private ushort[] PopArguments(int argumentCount)
	{
		ushort[] arguments = new ushort[argumentCount];
		for (int index = argumentCount - 1; index >= 0; index--)
		{
			arguments[index] = Pop();
		}

		return arguments;
	}

	private void RestoreEvaluationStackDepth(int depth)
	{
		while (evaluationStack.Count > depth)
		{
			evaluationStack.Pop();
		}
	}

	private static void ApplyInitializers(ProcedureEntry procedure, ushort[] locals)
	{
		foreach (ProcedureInitializer initializer in procedure.Header.Initializers)
		{
			if (initializer.LocalIndex >= locals.Length)
			{
				throw new LinchpinException($"Procedure initializer targets local {initializer.LocalIndex}, but the callee only declares {locals.Length} locals.");
			}

			locals[initializer.LocalIndex] = initializer.Value;
		}
	}

	private void RestoreJumpSnapshot(ushort token, ushort? returnValue, bool useReturnTarget)
	{
		if (!jumpSnapshots.TryGetValue(token, out JumpSnapshot? snapshot))
		{
			throw new LinchpinException($"LONGJMP references unknown SETJMP token 0x{token:X4}.");
		}

		CurrentModuleId = snapshot.ModuleId;
		ProgramCounter = useReturnTarget ? snapshot.LongJumpReturnProgramCounter : snapshot.LongJumpProgramCounter;
		tupleStackByteOffset = snapshot.TupleStackByteOffset;
		CurrentFrame = CloneFrame(snapshot.Frame);
		FrameUpperBound = snapshot.FrameUpperBound;

		evaluationStack.Clear();
		foreach (ushort value in snapshot.EvaluationStack)
		{
			evaluationStack.Push(value);
		}

		callStack.Clear();
		foreach (CallContinuation continuation in snapshot.CallStack)
		{
			callStack.Push(CloneContinuation(continuation));
		}

		if (returnValue.HasValue)
		{
			Push(returnValue.Value);
		}
	}

	private static VmFrame CloneFrame(VmFrame frame)
	{
		return frame with { Locals = frame.Locals.ToArray() };
	}

	private static CallContinuation CloneContinuation(CallContinuation continuation)
	{
		return continuation with { Frame = CloneFrame(continuation.Frame) };
	}

	private int ComputeFrameUpperBound(int moduleId, int procedureStartOffset)
	{
		Dictionary<int, int> upperBoundsForModule = frameUpperBoundsByProcedureStart[moduleId - 1];
		if (upperBoundsForModule.TryGetValue(procedureStartOffset, out int upperBound))
		{
			return upperBound;
		}

		ModuleImage module = image.Modules[moduleId - 1];
		int computedUpperBound = module.Length;
		for (int index = 0; index < module.Procedures.Count; index++)
		{
			ProcedureEntry candidate = module.Procedures[index];
			if (candidate.StartOffset > procedureStartOffset && candidate.StartOffset < computedUpperBound)
			{
				computedUpperBound = candidate.StartOffset;
			}
		}

		upperBoundsForModule[procedureStartOffset] = computedUpperBound;
		return computedUpperBound;
	}

	private static Dictionary<int, ProcedureEntry>[] BuildProcedureLookupTables(CornerstoneImage image, Func<ProcedureEntry, int> keySelector)
	{
		Dictionary<int, ProcedureEntry>[] lookupTables = new Dictionary<int, ProcedureEntry>[image.Modules.Count];
		for (int moduleIndex = 0; moduleIndex < image.Modules.Count; moduleIndex++)
		{
			ModuleImage module = image.Modules[moduleIndex];
			Dictionary<int, ProcedureEntry> lookup = new(module.Procedures.Count);
			for (int procedureIndex = 0; procedureIndex < module.Procedures.Count; procedureIndex++)
			{
				ProcedureEntry procedure = module.Procedures[procedureIndex];
				lookup[keySelector(procedure)] = procedure;
			}

			lookupTables[moduleIndex] = lookup;
		}

		return lookupTables;
	}

	private static Dictionary<int, int>[] BuildFrameUpperBoundCaches(CornerstoneImage image)
	{
		Dictionary<int, int>[] caches = new Dictionary<int, int>[image.Modules.Count];
		for (int moduleIndex = 0; moduleIndex < image.Modules.Count; moduleIndex++)
		{
			ModuleImage module = image.Modules[moduleIndex];
			Dictionary<int, int> cache = new(module.Procedures.Count);
			for (int procedureIndex = 0; procedureIndex < module.Procedures.Count; procedureIndex++)
			{
				ProcedureEntry procedure = module.Procedures[procedureIndex];
				int upperBound = procedureIndex + 1 < module.Procedures.Count
					? module.Procedures[procedureIndex + 1].StartOffset
					: module.Length;
				cache[procedure.StartOffset] = upperBound;
			}

			caches[moduleIndex] = cache;
		}

		return caches;
	}

	private void InitializeSystemSlots()
	{
		systemModuleGlobals[0xC7] = 79;  // screen width - 1
		systemModuleGlobals[0xC8] = 24;  // screen height - 1
		systemModuleGlobals[0xC9] = 0;
		systemModuleGlobals[0xCA] = 0;
		InitializeClockSystemSlots();
		systemModuleGlobals[0xD7] = EncodeExtensionToken("DBF");
		InitializeObjChannel();
	}

	private void InitializeClockSystemSlots()
	{
		(ushort month, ushort day, ushort year) = Host.GetCurrentDate();
		(ushort hour, ushort minute, ushort second) = Host.GetCurrentTime();
		systemModuleGlobals[0xCD] = month;
		systemModuleGlobals[0xCE] = day;
		systemModuleGlobals[0xCF] = year;
		systemModuleGlobals[0xD0] = hour;
		systemModuleGlobals[0xD1] = minute;
		systemModuleGlobals[0xD2] = second;
	}

	private void InitializeObjChannel()
	{
		ushort channelId = CreateChannelId();
		openChannels[channelId] = new VmChannel("CORNER.OBJ", image.ObjPath, image.ObjectBytes, 0x02, 0);
		int codePageCount = image.HeaderWords[7];
		systemModuleGlobals[0xDA] = (ushort)((codePageCount << 5) | (channelId & 0x1F));
	}

	private static ushort EncodeExtensionToken(string extension)
	{
		if (extension.Length != 3 || extension.Any(character => character is < 'A' or > 'Z'))
		{
			throw new LinchpinException($"Extension token '{extension}' is not a three-letter uppercase code.");
		}

		int first = extension[0] - 'A';
		int second = extension[1] - 'A';
		int third = extension[2] - 'A';
		return (ushort)((first << 11) + (second * 0x2D) + third);
	}

	private sealed record CallContinuation(int ModuleId, int ReturnProgramCounter, VmFrame Frame, int FrameUpperBound, int EvaluationStackDepth);
}
