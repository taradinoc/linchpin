/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Globalization;
using System.Text;

namespace Linchpin;

/// <summary>
/// Runs Cornerstone VM bytecode by decoding and dispatching instructions in a loop until
/// the program halts, an instruction limit is reached, or a conditional stop triggers.
/// </summary>
internal static class VmExecutor
{
	private const ushort FalseSentinel = 0x8001;
	private const int DefaultInstructionLimit = 5_000_000;
	private static readonly int InstructionLimit = LoadInstructionLimit();

	/// <summary>
	/// Executes a Cornerstone VM program to completion (or until a stop condition).
	/// </summary>
	/// <param name="image">The loaded Cornerstone image containing bytecode and metadata.</param>
	/// <param name="grammar">The opcode grammar for instruction decoding.</param>
	/// <param name="inputText">Pre-loaded keyboard input text for the program.</param>
	/// <param name="displayMode">Whether to render output as a transcript or on a live console.</param>
	/// <param name="executionOptions">Optional runtime settings (file I/O, snapshots, data directory).</param>
	/// <returns>The execution result including halt code, instruction count, and screen text.</returns>
	public static VmExecutionResult Run(
		CornerstoneImage image,
		InstructionGrammar grammar,
		string inputText,
		VmRunDisplayMode displayMode,
		VmExecutionOptions executionOptions = default)
	{
		VmRuntimeState state = VmRuntimeState.Create(image, inputText, displayMode, executionOptions);
		bool enforceInstructionLimit = displayMode != VmRunDisplayMode.LiveConsole;
		try
		{
			int instructionCount = 0;
			Queue<string> recentInstructions = new();

			while (!state.IsHalted)
			{
				if (enforceInstructionLimit && instructionCount >= InstructionLimit)
				{
					string screenDump = state.Host.Render();
					if (screenDump.Length > 0)
					{
						Console.Error.WriteLine("Screen at execution limit:");
						Console.Error.WriteLine(screenDump);
					}

					if (executionOptions.AllowInstructionLimitSnapshot)
					{
						Console.Error.WriteLine($"Execution limit reached before HALT after {InstructionLimit:N0} instructions; returning snapshot result.");
						return new VmExecutionResult(
							state.HaltCode,
							instructionCount,
							screenDump,
							true,
							VmExecutionStopReason.InstructionLimit,
							$"instruction limit {InstructionLimit:N0}");
					}

					throw new LinchpinException($"Execution limit exceeded before HALT at {DescribeExecutionPosition(state)} after {InstructionLimit:N0} instructions. Recent instructions: {string.Join(" | ", recentInstructions)}");
				}

				ModuleImage module = image.Modules[state.CurrentModuleId - 1];
				int upperBound = state.FrameUpperBound;
				DecodedInstruction instruction = BytecodeDecoder.DecodeInstructionAt(image, grammar, module, state.ProgramCounter, upperBound);
				AppendRecentInstruction(recentInstructions, state, instruction);

				try
				{
					ExecuteInstruction(state, instruction, instructionCount);
				}
				catch (LinchpinException exception)
				{
					throw new LinchpinException(
						$"Execution failed in module {state.CurrentModuleId} at 0x{state.ProgramCounter:X4} on {instruction.Mnemonic}: {exception.Message}");
				}
				instructionCount++;
				if (TryCreateConditionalStopResult(state, instructionCount, out VmExecutionResult? conditionalStopResult))
				{
					return conditionalStopResult!;
				}
			}

			return new VmExecutionResult(
				state.HaltCode,
				instructionCount,
				state.Host.Render(),
				false,
				VmExecutionStopReason.Halt,
				null);
		}
		finally
		{
			state.Host.Dispose();
		}
	}

	private static int LoadInstructionLimit()
	{
		string? rawValue = Environment.GetEnvironmentVariable("LINCHPIN_EXECUTION_LIMIT");
		if (string.IsNullOrWhiteSpace(rawValue))
		{
			return DefaultInstructionLimit;
		}

		return int.TryParse(rawValue, out int parsedValue) && parsedValue > 0
			? parsedValue
			: DefaultInstructionLimit;
	}

	private static void AppendRecentInstruction(Queue<string> recentInstructions, VmRuntimeState state, DecodedInstruction instruction)
	{
		string operands = instruction.Operands.Count == 0
			? string.Empty
			: " " + string.Join(", ", instruction.Operands.Select(operand => operand.DisplayText));

		recentInstructions.Enqueue($"m{state.CurrentModuleId}:p{state.CurrentFrame.ProcedureIndex}:0x{state.ProgramCounter:X4}:{instruction.Mnemonic}{operands}");
		while (recentInstructions.Count > 16)
		{
			recentInstructions.Dequeue();
		}
	}

	private static string DescribeExecutionPosition(VmRuntimeState state) => state.BuildExecutionSummary();

	private static bool TryCreateConditionalStopResult(VmRuntimeState state, int instructionCount, out VmExecutionResult? result)
	{
		VmTraceConfiguration configuration = VmTraceSession.Configuration;
		if (configuration.StopOnOutputPattern is string outputPattern && state.Host.ContainsRenderedText(outputPattern))
		{
			string screenDump = state.Host.Render();
			Console.Error.WriteLine(
				$"Execution stopped after matching screen text '{outputPattern}' at {DescribeExecutionPosition(state)} after {instructionCount:N0} instructions.");
			result = new VmExecutionResult(
				state.HaltCode,
				instructionCount,
				screenDump,
				false,
				VmExecutionStopReason.OutputPattern,
				$"output pattern '{outputPattern}'");
			return true;
		}

		if (configuration.StopAfterKbInputCount is int kbInputLimit && state.KbInputEventCount >= kbInputLimit)
		{
			string screenDump = state.Host.Render();
			Console.Error.WriteLine(
				$"Execution stopped after {state.KbInputEventCount:N0} KBINPUT events at {DescribeExecutionPosition(state)} after {instructionCount:N0} instructions.");
			result = new VmExecutionResult(
				state.HaltCode,
				instructionCount,
				screenDump,
				false,
				VmExecutionStopReason.KbInputCount,
				$"KBINPUT count {state.KbInputEventCount:N0}");
			return true;
		}

		result = null;
		return false;
	}

	/// <summary>
	/// Dispatches a single decoded instruction, mutating VM state accordingly.
	/// </summary>
	private static void ExecuteInstruction(VmRuntimeState state, DecodedInstruction instruction, int instructionCount)
	{
		int nextProgramCounter = state.ProgramCounter + instruction.ByteLength;
		state.TraceInstructionExecution(instruction, instructionCount);

		switch (instruction.Mnemonic)
		{
			case "CALL0":
				state.EnterNearCall((ushort)instruction.Operands[0].RawValue, 0, nextProgramCounter);
				return;
			case "CALL1":
				state.EnterNearCall((ushort)instruction.Operands[0].RawValue, 1, nextProgramCounter);
				return;
			case "CALL2":
				state.EnterNearCall((ushort)instruction.Operands[0].RawValue, 2, nextProgramCounter);
				return;
			case "CALL3":
				state.EnterNearCall((ushort)instruction.Operands[0].RawValue, 3, nextProgramCounter);
				return;
			case "CALL":
			{
				ushort callee = state.Pop();
				state.EnterNearCall(callee, instruction.Operands[0].RawValue, nextProgramCounter);
				return;
			}
			case "CALLF0":
				state.EnterFarCall((ushort)instruction.Operands[0].RawValue, 0, nextProgramCounter);
				return;
			case "CALLF1":
				state.EnterFarCall((ushort)instruction.Operands[0].RawValue, 1, nextProgramCounter);
				return;
			case "CALLF2":
				state.EnterFarCall((ushort)instruction.Operands[0].RawValue, 2, nextProgramCounter);
				return;
			case "CALLF3":
				state.EnterFarCall((ushort)instruction.Operands[0].RawValue, 3, nextProgramCounter);
				return;
			case "CALLF":
			{
				ushort selector = state.Pop();
				state.EnterFarCall(selector, instruction.Operands[0].RawValue, nextProgramCounter);
				return;
			}
			case "PUSH_NIL":
				state.Push(FalseSentinel);
				break;
			case "PUSH0":
				state.Push(0);
				break;
			case "PUSH1":
				state.Push(1);
				break;
			case "PUSH2":
				state.Push(2);
				break;
			case "PUSH3":
				state.Push(3);
				break;
			case "PUSH4":
				state.Push(4);
				break;
			case "PUSH5":
				state.Push(5);
				break;
			case "PUSH6":
				state.Push(6);
				break;
			case "PUSH7":
				state.Push(7);
				break;
			case "PUSH8":
				state.Push(8);
				break;
			case "PUSHm1":
				state.Push(0xFFFF);
				break;
			case "PUSHm8":
				state.Push(0xFFF8);
				break;
			case "PUSHFF":
				state.Push(0x00FF);
				break;
			case "PUSH":
				state.Push((ushort)instruction.Operands[0].RawValue);
				break;
			case "DUP":
				state.Push(state.Peek());
				break;
			case "PUSHB":
			case "PUSHW":
				state.PushWordOperand(instruction, 0);
				break;
			case "ADD":
			case "ADD_":
			case "REST":
			{
				ushort right = state.Pop();
				ushort left = state.Pop();
				state.Push(unchecked((ushort)(left + right)));
				break;
			}
			case "MUL":
			{
				short right = unchecked((short)state.Pop());
				short left = unchecked((short)state.Pop());
				state.Push(unchecked((ushort)(left * right)));
				break;
			}
			case "INCL":
			{
				int localIndex = instruction.Operands[0].RawValue;
				ushort updated = unchecked((ushort)(state.LoadLocal(localIndex) + 1));
				state.StoreLocal(localIndex, updated, preserveTop: false);
				state.Push(updated);
				break;
			}
			case "DECL":
			{
				int localIndex = instruction.Operands[0].RawValue;
				ushort updated = unchecked((ushort)(state.LoadLocal(localIndex) - 1));
				state.StoreLocal(localIndex, updated, preserveTop: false);
				state.Push(updated);
				break;
			}
			case "SUB":
			{
				ushort right = state.Pop();
				ushort left = state.Pop();
				state.Push(unchecked((ushort)(left - right)));
				break;
			}
			case "NEG":
			{
				short value = unchecked((short)state.Pop());
				state.Push(unchecked((ushort)(-value)));
				break;
			}
			case "DIV":
			{
				short divisor = unchecked((short)state.Pop());
				short dividend = unchecked((short)state.Pop());
				state.Push(divisor == 0 ? (ushort)0 : unchecked((ushort)(dividend / divisor)));
				break;
			}
			case "AND":
				state.Push((ushort)(state.Pop() & state.Pop()));
				break;
			case "OR":
				state.Push((ushort)(state.Pop() | state.Pop()));
				break;
			case "XOR":
				state.Push((ushort)(state.Pop() ^ state.Pop()));
				break;
			case "NOT":
				state.Push((ushort)~state.Pop());
				break;
			case "MOD":
			{
				// MME.EXE uses IDIV (signed) then adjusts a negative remainder
				// by adding the divisor, producing a floored/Euclidean modulus.
				short divisor = unchecked((short)state.Pop());
				short dividend = unchecked((short)state.Pop());
				if (divisor == 0)
				{
					state.Push(0);
				}
				else
				{
					short remainder = (short)(dividend % divisor);
					if (remainder < 0)
						remainder += divisor;
					state.Push(unchecked((ushort)remainder));
				}
				break;
			}
			case "SHIFT":
			{
				short shift = unchecked((short)state.Pop());
				ushort value = state.Pop();
				state.Push(ApplyLogicalShift(value, shift));
				break;
			}
			case "ASHIFT":
			{
				short shift = unchecked((short)state.Pop());
				short value = unchecked((short)state.Pop());
				state.Push(ApplyArithmeticShift(value, shift));
				break;
			}
			case "PUTMG":
			{
				ushort mgValue = state.Pop();
				int mgIndex = instruction.Operands[0].RawValue;
				state.StoreModuleGlobal(mgIndex, mgValue);
				break;
			}
			case "LOADMG":
				state.Push(state.LoadModuleGlobal(instruction.Operands[0].RawValue));
				break;
			case "LOADG":
				state.Push(state.LoadProgramGlobal(instruction.Operands[0].RawValue));
				break;
			case "INCG":
			{
				int globalIndex = instruction.Operands[0].RawValue;
				ushort updated = unchecked((ushort)(state.LoadProgramGlobal(globalIndex) + 1));
				state.StoreProgramGlobal(globalIndex, updated);
				state.Push(updated);
				break;
			}
			case "INCMG":
			{
				int globalIndex = instruction.Operands[0].RawValue;
				ushort updated = unchecked((ushort)(state.LoadModuleGlobal(globalIndex) + 1));
				state.StoreModuleGlobal(globalIndex, updated);
				state.Push(updated);
				break;
			}
			case "DECG":
			{
				int globalIndex = instruction.Operands[0].RawValue;
				ushort updated = unchecked((ushort)(state.LoadProgramGlobal(globalIndex) - 1));
				state.StoreProgramGlobal(globalIndex, updated);
				state.Push(updated);
				break;
			}
			case "DECMG":
			{
				int globalIndex = instruction.Operands[0].RawValue;
				ushort updated = unchecked((ushort)(state.LoadModuleGlobal(globalIndex) - 1));
				state.StoreModuleGlobal(globalIndex, updated);
				state.Push(updated);
				break;
			}
			case "PUTG":
				state.StoreProgramGlobal(instruction.Operands[0].RawValue, state.Pop());
				break;
			case "STOREMG":
				state.StoreModuleGlobal(instruction.Operands[0].RawValue, state.Peek());
				break;
			case "STOREG":
				state.StoreProgramGlobal(instruction.Operands[0].RawValue, state.Peek());
				break;
			case "VLOADW":
			{
				if (instruction.Operands.Count == 0)
				{
					ushort index = state.Pop();
					ushort handle = state.Pop();
					int resolvedIndex = state.ResolveDynamicWordReadIndex(handle, index);
					ushort value = state.ReadAggregateWord(handle, resolvedIndex);
					state.Push(value);
					break;
				}

				if (instruction.Operands.Count == 1)
				{
					ushort handle = state.Pop();
					int wordIdx = instruction.Operands[0].RawValue;
					ushort readVal = state.ReadAggregateWord(handle, wordIdx);
					state.Push(readVal);
					break;
				}

				int wordIndex = instruction.Operands[0].RawValue;
				int localIndex = instruction.Operands[1].RawValue;
				ushort localHandle = state.LoadLocal(localIndex);
				ushort packedValue = state.ReadAggregateWord(localHandle, wordIndex);
				state.Push(packedValue);
				break;
			}
			case "VLOADB":
			{
				if (instruction.Operands.Count == 0)
				{
					int index = unchecked((short)state.Pop());
					ushort dynamicHandle = state.Pop();
					state.Push(state.ReadAggregateByte(dynamicHandle, index - 1));
					break;
				}

				ushort immediateHandle = state.Pop();
				state.Push(state.ReadAggregateByte(immediateHandle, instruction.Operands[0].RawValue));
				break;
			}
			case "LOADVB2":
			{
				int rawIndex = unchecked((short)state.Pop());
				ushort handle = state.Pop();
				state.Push(state.ReadAggregateRawByte(handle, rawIndex - 1));
				break;
			}
			case "VPUTW":
			{
				if (instruction.Operands.Count == 1)
				{
					ushort immediateValue = state.Pop();
					ushort immediateHandle = state.Pop();
					state.WriteAggregateWord(immediateHandle, instruction.Operands[0].RawValue, immediateValue);
					break;
				}

				ushort value = state.Pop();
				ushort index = state.Pop();
				ushort handle = state.Pop();
				state.WriteAggregateWord(handle, state.ResolveDynamicWordReadIndex(handle, index), value);
				break;
			}
			case "VPUTB":
			{
				ushort value = state.Pop();
				int index = unchecked((short)state.Pop());
				ushort handle = state.Pop();
				state.WriteAggregateByte(handle, index - 1, (byte)(value & 0xFF));
				break;
			}
			case "VPUTW_":
				throw new LinchpinException("VPUTW_ should have been normalized to VPUTW during decoding.");
			case "VPUTB_":
			{
				ushort value = state.Pop();
				ushort handle = state.Pop();
				state.WriteAggregateByte(handle, instruction.Operands[0].RawValue, (byte)(value & 0xFF));
				break;
			}
			case "PUTVB2":
			case "POPVB2":
			{
				ushort value = state.Pop();
				int rawIndex = unchecked((short)state.Pop());
				ushort handle = state.Pop();
				state.WriteAggregateRawByte(handle, rawIndex - 1, (byte)(value & 0xFF));
				break;
			}
			case "VALLOC":
				state.Push(state.AllocateVector(state.Pop()));
				break;
			case "VALLOCI":
			{
				ushort wordCount = state.Pop();
				ushort[] values = new ushort[wordCount];
				for (int index = values.Length - 1; index >= 0; index--)
				{
					values[index] = state.Pop();
				}

				state.Push(state.AllocateVector(wordCount, values));
				break;
			}
			case "VFREE":
			{
				ushort size = state.Pop();
				ushort handle = state.Pop();
				state.ReleaseAggregate(handle, size);
				break;
			}
			case "TALLOC":
				state.Push(state.AllocateTuple(state.Pop()));
				break;
			case "TALLOCI":
			{
				ushort wordCount = state.Pop();
				ushort[] values = new ushort[wordCount];
				for (int index = values.Length - 1; index >= 0; index--)
				{
					values[index] = state.Pop();
				}

				state.Push(state.AllocateTuple(wordCount, values));
				break;
			}
			case "VECSETW":
			{
				ushort value = state.Pop();
				ushort count = state.Pop();
				ushort handle = state.Pop();
				state.FillAggregateWords(handle, count, value);
				state.Push(handle);
				break;
			}
			case "VECSETB":
			{
				byte value = (byte)(state.Pop() & 0xFF);
				ushort count = state.Pop();
				ushort handle = state.Pop();
				state.FillAggregateBytesUnclamped(handle, count, value);
				state.Push(handle);
				break;
			}
			case "VECCPYW":
			{
				ushort destinationHandle = state.Pop();
				ushort count = state.Pop();
				ushort sourceHandle = state.Pop();
				state.CopyAggregateWords(sourceHandle, destinationHandle, count);
				state.Push(destinationHandle);
				break;
			}
			case "VECCPYB":
			{
				int destinationOffset = unchecked((short)state.Pop());
				int sourceOffset = unchecked((short)state.Pop());
				ushort destinationHandle = state.Pop();
				ushort count = state.Pop();
				ushort sourceHandle = state.Pop();
				try
				{
					state.CopyAggregateBytesUnclamped(sourceHandle, sourceOffset, destinationHandle, destinationOffset, count);
				}
				catch (LinchpinException exception)
				{
					throw new LinchpinException(
						$"VECCPYB failed in module {state.CurrentModuleId} at 0x{state.ProgramCounter:X4} with src=0x{sourceHandle:X4}, count=0x{count:X4}, dst=0x{destinationHandle:X4}, srcOff={sourceOffset}, dstOff={destinationOffset}: {exception.Message}");
				}
				state.Push(destinationHandle);
				break;
			}
			case "SETJMP":
				state.Push(state.CreateSetJumpToken(instruction.Operands[0].RawValue, instruction.Operands[1].RawValue));
				break;
			case "PINM":
			case "UNPINM":
			case "EXT_31":
			case "EXT_32":
				break;
			case "LONGJMPR":
			{
				ushort returnValue = state.Pop();
				ushort token = state.Pop();
				state.LongJumpReturn(token, returnValue);
				return;
			}
			case "LONGJMP":
				state.LongJump(state.Pop());
				return;
			case "BITSVL":
				state.Push(state.ExtractBitFieldFromLocal((ushort)instruction.Operands[0].RawValue));
				break;
			case "BITSV":
			{
				ushort handle = state.Pop();
				state.Push(state.ExtractBitField(handle, (ushort)instruction.Operands[0].RawValue));
				break;
			}
			case "BBSETVL":
				state.ReplaceBitFieldInLocal((ushort)instruction.Operands[0].RawValue, state.Pop());
				break;
			case "BBSETV":
			{
				ushort handle = state.Pop();
				ushort value = state.Pop();
				state.ReplaceBitField(handle, (ushort)instruction.Operands[0].RawValue, value);
				break;
			}
			case "BSETVL":
				state.ReplaceSingleBitInLocal((ushort)instruction.Operands[0].RawValue);
				break;
			case "BSETV":
			{
				ushort handle = state.Pop();
				state.ReplaceSingleBit(handle, (ushort)instruction.Operands[0].RawValue);
				break;
			}
			case "INCLV":
			{
				int localIndex = instruction.Operands[0].RawValue;
				ushort current = state.LoadLocal(localIndex);
				ushort updated = current == FalseSentinel ? FalseSentinel : unchecked((ushort)(current + 1));
				state.StoreLocal(localIndex, updated, preserveTop: false);
				break;
			}
			case "PRINTV":
			{
				ushort startOffset = state.Pop();
				ushort length = state.Pop();
				ushort handle = state.Pop();
				state.Host.PrintVector(state, handle, length, startOffset);
				break;
			}
			case "PRCHAR":
				state.Host.PrintCharacter((char)(state.Pop() & 0xFF));
				break;
			case "WPRINTV":
			case "EXT_21":
			{
				ushort sourceOffset = state.Pop();
				ushort charCount = state.Pop();
				ushort sourceHandle = state.Pop();
				ushort descriptorHandle = state.Pop();
				state.Push(state.ExecuteWprintv(descriptorHandle, sourceHandle, charCount, sourceOffset));
				break;
			}
			case "SETWIN":
			case "EXT_22":
				state.Push(state.ExecuteSetwin(state.Pop()));
				break;
			case "FADD":
				state.Push(ExecuteBinaryRealOperation(state, static (left, right) => left + right));
				break;
			case "FSUB":
				state.Push(ExecuteBinaryRealOperation(state, static (left, right) => left - right));
				break;
			case "FMUL":
				state.Push(ExecuteBinaryRealOperation(state, static (left, right) => left * right));
				break;
			case "FDIV":
				state.Push(ExecuteBinaryRealOperation(state, static (left, right) => left / right));
				break;
			case "FLOG":
				state.Push(ExecuteUnaryRealOperation(state, Math.Log));
				break;
			case "FEXP":
				state.Push(ExecuteUnaryRealOperation(state, Math.Exp));
				break;
			case "PRSREAL":
			case "EXT_35":
				state.Push(ExecutePrsreal(state));
				break;
			case "LOOKUP":
				if (state.TryExecuteLookup(nextProgramCounter, out ushort lookupResult))
				{
					state.Push(lookupResult);
					break;
				}

				return;
			case "EXTRACT":
			{
				ushort extractResult = state.ExecuteExtract();
				state.Push(extractResult);
				break;
			}
			case "UNPACK":
				state.Push(state.ExecuteUnpack());
				break;
			case "KBINPUT":
			{
				bool suppressed = state.IsInputSuppressed();
				ushort kbResult = suppressed ? FalseSentinel : state.Host.PollKey();
				state.RecordKbInputEvent(kbResult);
				state.Push(kbResult);
				break;
			}
			case "UNLINK":
				state.Push(state.UnlinkFile(state.Pop()));
				break;
			case "STRCHR":
			{
				ushort handle = state.Pop();
				ushort character = state.Pop();
				state.Push(state.FindCharacterInVmString(handle, (byte)(character & 0xFF)));
				break;
			}
			case "MEMCMP":
			{
				ushort byteCount = state.Pop();
				ushort firstHandle = state.Pop();
				ushort secondHandle = state.Pop();
				state.Push(state.CompareAggregateBytes(firstHandle, 0, secondHandle, 0, byteCount));
				break;
			}
			case "MEMCMPO":
			{
				ushort firstOffset = state.Pop();
				ushort byteCount = state.Pop();
				ushort firstHandle = state.Pop();
				ushort secondHandle = state.Pop();
				ushort memcmpResult = state.CompareAggregateRawBytesToPayload(firstHandle, firstOffset, secondHandle, 0, byteCount);
				state.Push(memcmpResult);
				break;
			}
			case "KEYCMP":
			{
				ushort secondHandle = state.Pop();
				ushort firstHandle = state.Pop();
				state.Push(state.CompareSortKeys(firstHandle, secondHandle));
				break;
			}
			case "STRICMP":
			{
				ushort firstLength = state.Pop();
				ushort firstOffset = state.Pop();
				ushort secondLength = state.Pop();
				ushort firstHandle = state.Pop();
				ushort secondHandle = state.Pop();
				ushort result = state.CompareVmStringsIgnoreCase(firstHandle, firstOffset, firstLength, secondHandle, secondLength);
				state.Push(result);
				break;
			}
			case "STRICMP1":
			{
				ushort firstLength = state.Pop();
				ushort firstOffset = state.Pop();
				ushort secondLength = state.Pop();
				ushort firstHandle = state.Pop();
				ushort secondHandle = state.Pop();
				ushort adjustedFirst = unchecked((ushort)(firstHandle - 1));
				ushort result = state.CompareVmStringsIgnoreCase(adjustedFirst, firstOffset, firstLength, secondHandle, secondLength);
				state.Push(result);
				break;
			}
			case "OPEN":
				state.Push(state.OpenSyntheticChannel(instruction.Operands[0].RawValue));
				break;
			case "CLOSE":
				state.Push(state.CloseSyntheticChannel(state.Pop()));
				break;
			case "FSIZE":
			{
				ushort fsizeChan = state.Pop();
				ushort fsizeResult = state.GetChannelSizeBlocks(fsizeChan);
				state.Push(fsizeResult);
				break;
			}
			case "READ":
			{
				ushort byteCount = state.Pop();
				ushort channelId = state.Pop();
				ushort vectorHandle = state.Pop();
				state.Push(state.ReadChannel(vectorHandle, channelId, byteCount));
				break;
			}
			case "WRITE":
			{
				ushort byteCount = state.Pop();
				ushort channelId = state.Pop();
				ushort vectorHandle = state.Pop();
				state.Push(state.WriteChannel(vectorHandle, channelId, byteCount));
				break;
			}
			case "READREC":
			{
				ushort wordCount = state.Pop();
				ushort record = state.Pop();
				ushort channelId = state.Pop();
				ushort vectorHandle = state.Pop();
				state.Push(state.ReadRecord(vectorHandle, channelId, record, wordCount));
				break;
			}
			case "WRITEREC":
			{
				ushort wordCount = state.Pop();
				ushort record = state.Pop();
				ushort channelId = state.Pop();
				ushort vectorHandle = state.Pop();
				state.Push(state.WriteRecord(vectorHandle, channelId, record, wordCount));
				break;
			}
			case "PULLRET":
			case "POPRET":
				state.DiscardReturnBundle();
				break;
			case "TPOP":
				state.ReleaseTupleWords(state.Pop());
				break;
			case "DISP":
				state.Host.ApplyDisplaySuboperation(instruction.Operands[0].RawValue);
				break;
			case "XDISP":
			{
				ushort argument = state.Pop();
				state.Host.ApplyExtendedDisplaySuboperation(instruction.Operands[0].RawValue, argument);
				state.Push(argument);
				break;
			}
			case "JUMPF":
				if (state.Pop() == FalseSentinel)
				{
					nextProgramCounter = instruction.Operands[0].RawValue;
				}
				break;
			case "JUMPGZ":
				if (unchecked((short)state.Pop()) > 0)
				{
					nextProgramCounter = instruction.Operands[0].RawValue;
				}
				break;
			case "JUMPLEZ":
				if (unchecked((short)state.Pop()) <= 0)
				{
					nextProgramCounter = instruction.Operands[0].RawValue;
				}
				break;
			case "JUMPLZ":
				if (unchecked((short)state.Pop()) < 0)
				{
					nextProgramCounter = instruction.Operands[0].RawValue;
				}
				break;
			case "JUMPGEZ":
				if (unchecked((short)state.Pop()) >= 0)
				{
					nextProgramCounter = instruction.Operands[0].RawValue;
				}
				break;
			case "JUMPNZ":
				if (state.Pop() != 0)
				{
					nextProgramCounter = instruction.Operands[0].RawValue;
				}
				break;
			case "JUMPZ":
				if (state.Pop() == 0)
				{
					nextProgramCounter = instruction.Operands[0].RawValue;
				}
				break;
			case "JUMPNF":
				if (state.Pop() != FalseSentinel)
				{
					nextProgramCounter = instruction.Operands[0].RawValue;
				}
				break;
			case "JUMPEQ":
			{
				ushort right = state.Pop();
				ushort left = state.Pop();
				if (left == right)
				{
					nextProgramCounter = instruction.Operands[0].RawValue;
				}
				break;
			}
			case "JUMPNE":
			{
				ushort right = state.Pop();
				ushort left = state.Pop();
				if (left != right)
				{
					nextProgramCounter = instruction.Operands[0].RawValue;
				}
				break;
			}
			case "JUMPL2":
			case "JUMPL":
			{
				short top = unchecked((short)state.Pop());
				short below = unchecked((short)state.Pop());
				if (top < below)
				{
					nextProgramCounter = instruction.Operands[0].RawValue;
				}
				break;
			}
			case "JUMPLE2":
			case "JUMPLE":
			{
				short top = unchecked((short)state.Pop());
				short below = unchecked((short)state.Pop());
				if (top <= below)
				{
					nextProgramCounter = instruction.Operands[0].RawValue;
				}
				break;
			}
			case "JUMPGE2":
			case "JUMPGE":
			{
				short top = unchecked((short)state.Pop());
				short below = unchecked((short)state.Pop());
				if (top >= below)
				{
					nextProgramCounter = instruction.Operands[0].RawValue;
				}
				break;
			}
			case "JUMPG2":
			case "JUMPG":
			{
				short top = unchecked((short)state.Pop());
				short below = unchecked((short)state.Pop());
				if (top > below)
				{
					nextProgramCounter = instruction.Operands[0].RawValue;
				}
				break;
			}
			case "JUMP":
				nextProgramCounter = instruction.Operands[0].RawValue;
				break;
			case "HALT":
				state.Halt((ushort)instruction.Operands[0].RawValue);
				break;
			case "PUSHL":
				state.Push(state.LoadLocal(instruction.Operands[0].RawValue));
				break;
			case "PUTL":
				state.StoreLocal(instruction.Operands[0].RawValue, state.Pop(), preserveTop: false);
				break;
			case "STOREL":
				state.StoreLocal(instruction.Operands[0].RawValue, state.Peek(), preserveTop: true);
				break;
			case "PULL":
			case "POP":
			{
				int count = instruction.Operands.Count == 0 ? 1 : instruction.Operands[0].RawValue;
				for (int index = 0; index < count; index++)
				{
					state.Pop();
				}

				break;
			}
			case "PULL_":
			case "POPI":
			{
				for (int index = 0; index < instruction.Operands[0].RawValue; index++)
				{
					state.Pop();
				}

				break;
			}
			case "VECL":
			case "ADVANCE":
			{
				int offset = unchecked((short)state.Pop());
				ushort handle = state.Pop();
				int keyLength = state.ReadAggregateRawByte(handle, offset - 1);
				int dataLength = state.ReadAggregateRawByte(handle, offset + keyLength + 1);
				ushort advance = unchecked((ushort)(keyLength + dataLength + 3));
				state.Push(unchecked((ushort)(offset + advance)));
				break;
			}
			case "FMTREAL":
			{
				ushort controlWord1 = state.Pop();
				ushort controlWord0 = state.Pop();
				ushort destinationWord = state.Pop();
				ushort sourceWord = state.Pop();

				if (sourceWord != 0 && sourceWord != FalseSentinel && destinationWord != 0 && destinationWord != FalseSentinel)
				{
					string formatted = FormatFmtrealNumericText(state, sourceWord, controlWord0, controlWord1);
					int capacity = state.ReadAggregateWord(destinationWord, 0);
					int bytesToWrite = Math.Min(formatted.Length, capacity);
					for (int index = 0; index < bytesToWrite; index++)
					{
						state.WriteAggregateByte(destinationWord, index, (byte)formatted[index]);
					}

					if (bytesToWrite < capacity)
					{
						state.WriteAggregateByte(destinationWord, bytesToWrite, 0);
					}

					state.Push((ushort)bytesToWrite);
				}
				else
				{
					state.Push(FormatFmtrealScalar(sourceWord, destinationWord, controlWord0, controlWord1));
				}
				break;
			}
			case "RET":
				state.Return(Array.Empty<ushort>());
				return;
			case "RETURN":
			{
				ushort retVal = state.Pop();
				state.Return(new[] { retVal });
				return;
			}
			case "RFALSE":
				state.Return(new[] { FalseSentinel });
				return;
			case "RZERO":
				state.Return(new[] { (ushort)0 });
				return;
			case "RET_":
			case "RETN":
			{
				int returnCount = instruction.Operands[0].RawValue;
				ushort[] resultWords = new ushort[returnCount];
				for (int index = returnCount - 1; index >= 0; index--)
				{
					resultWords[index] = state.Pop();
				}

				state.Return(resultWords);
				return;
			}
			case "NEXTB":
				break;
			default:
				throw new LinchpinException($"Opcode '{instruction.Mnemonic}' is not implemented yet in the runnable interpreter subset.");
		}

		state.ProgramCounter = nextProgramCounter;
	}

	private static ushort SignExtendByte(ushort value)
	{
		return unchecked((ushort)(short)(sbyte)(byte)value);
	}

	private static ushort ApplyLogicalShift(ushort value, short shift)
	{
		if (shift >= 0)
		{
			return unchecked((ushort)(value << shift));
		}

		return (ushort)(value >> -shift);
	}

	private static ushort ApplyArithmeticShift(short value, short shift)
	{
		if (shift >= 0)
		{
			return unchecked((ushort)(value << shift));
		}

		return unchecked((ushort)(value >> -shift));
	}

	private static ushort ExecuteBinaryRealOperation(VmRuntimeState state, Func<double, double, double> operation)
	{
		ushort destinationHandle = state.Pop();
		ushort rightHandle = state.Pop();
		ushort leftHandle = state.Pop();
		double left = ReadReal64(state, leftHandle);
		double right = ReadReal64(state, rightHandle);
		double result = operation(left, right);
		WriteReal64(state, destinationHandle, result);
		return destinationHandle;
	}

	private static ushort ExecuteUnaryRealOperation(VmRuntimeState state, Func<double, double> operation)
	{
		ushort destinationHandle = state.Pop();
		ushort sourceHandle = state.Pop();
		double value = ReadReal64(state, sourceHandle);
		WriteReal64(state, destinationHandle, operation(value));
		return destinationHandle;
	}

	private static ushort ExecutePrsreal(VmRuntimeState state)
	{
		ushort resultControlHandle = state.Pop();
		ushort optionWord = state.Pop();
		ushort sourceOffset = state.Pop();
		ushort sourceLength = state.Pop();
		ushort destinationHandle = state.Pop();
		ushort sourceHandle = state.Pop();

		ushort consumedCount = 0;
		ushort statusWord = 0;

		if (TryParsePrsrealText(state, sourceHandle, sourceOffset, sourceLength, optionWord, out double value, out consumedCount, out statusWord)
			&& destinationHandle != 0
			&& destinationHandle != FalseSentinel)
		{
			WriteReal64(state, destinationHandle, value);
			if (resultControlHandle != 0 && resultControlHandle != FalseSentinel)
			{
				state.WriteAggregateByte(resultControlHandle, 0, (byte)(consumedCount & 0xFF));
				state.WriteAggregateByte(resultControlHandle, 1, (byte)(statusWord & 0xFF));
			}

			return destinationHandle;
		}

		if (resultControlHandle != 0 && resultControlHandle != FalseSentinel)
		{
			state.WriteAggregateByte(resultControlHandle, 0, (byte)(consumedCount & 0xFF));
			state.WriteAggregateByte(resultControlHandle, 1, (byte)(statusWord & 0xFF));
		}

		return FalseSentinel;
	}

	private static double ReadReal64(VmRuntimeState state, ushort handle)
	{
		Span<byte> buffer = stackalloc byte[8];
		for (int index = 0; index < buffer.Length; index++)
		{
			buffer[index] = (byte)state.ReadAggregateByte(handle, index);
		}

		if (BitConverter.IsLittleEndian)
		{
			buffer.Reverse();
		}

		return BitConverter.Int64BitsToDouble(BitConverter.ToInt64(buffer));
	}

	private static void WriteReal64(VmRuntimeState state, ushort handle, double value)
	{
		Span<byte> buffer = stackalloc byte[8];
		BitConverter.TryWriteBytes(buffer, value);

		if (BitConverter.IsLittleEndian)
		{
			buffer.Reverse();
		}

		for (int index = 0; index < buffer.Length; index++)
		{
			state.WriteAggregateByte(handle, index, buffer[index]);
		}
	}

	private static bool TryParsePrsrealText(
		VmRuntimeState state,
		ushort sourceHandle,
		ushort sourceOffset,
		ushort sourceLength,
		ushort optionWord,
		out double value,
		out ushort consumedCount,
		out ushort statusWord)
	{
		value = 0;
		consumedCount = 0;
		statusWord = 0;
		_ = optionWord;

		if (sourceHandle == 0 || sourceHandle == FalseSentinel)
		{
			statusWord = 1;
			return false;
		}

		int declaredLength = state.ReadAggregateWord(sourceHandle, 0);
		int boundedOffset = Math.Min(sourceOffset, (ushort)declaredLength);
		int byteCount = Math.Min(sourceLength, (ushort)Math.Max(0, declaredLength - boundedOffset));
		if (byteCount <= 0)
		{
			statusWord = 1;
			return false;
		}

		StringBuilder builder = new(byteCount);
		for (int index = 0; index < byteCount; index++)
		{
			builder.Append((char)(byte)state.ReadAggregateByte(sourceHandle, boundedOffset + index));
		}

		return TryNormalizePrsrealText(builder.ToString(), out string normalized, out consumedCount, out statusWord)
			&& double.TryParse(normalized, NumberStyles.Float, CultureInfo.InvariantCulture, out value);
	}

	private static bool TryNormalizePrsrealText(
		string rawText,
		out string normalized,
		out ushort consumedCount,
		out ushort statusWord)
	{
		normalized = string.Empty;
		consumedCount = 0;
		statusWord = 0;

		if (string.IsNullOrWhiteSpace(rawText))
		{
			statusWord = 1;
			return false;
		}

		ReadOnlySpan<char> span = rawText.AsSpan();
		int index = 0;
		bool negative = false;
		bool usedParentheses = false;
		bool closedParentheses = false;
		bool sawSignificandDigit = false;
		bool sawDecimalPoint = false;
		bool sawExponent = false;
		bool sawExponentDigit = false;
		bool exponentSignAllowed = false;

		while (index < span.Length && char.IsWhiteSpace(span[index]))
		{
			index++;
		}

		if (index < span.Length && span[index] == '(')
		{
			usedParentheses = true;
			negative = true;
			index++;
		}

		while (index < span.Length)
		{
			char ch = span[index];
			if (ch == '$' || ch == ' ')
			{
				index++;
				continue;
			}

			if ((ch == '+' || ch == '-') && !sawSignificandDigit && !sawDecimalPoint && !sawExponent)
			{
				negative ^= ch == '-';
				index++;
				continue;
			}

			break;
		}

		StringBuilder builder = new(span.Length + 1);
		if (negative)
		{
			builder.Append('-');
		}

		for (; index < span.Length; index++)
		{
			char ch = span[index];

			if (char.IsWhiteSpace(ch))
			{
				break;
			}

			if (usedParentheses && ch == ')')
			{
				closedParentheses = true;
				index++;
				break;
			}

			if (ch == '$' || ch == ',')
			{
				continue;
			}

			if (char.IsDigit(ch))
			{
				builder.Append(ch);
				if (sawExponent)
				{
					sawExponentDigit = true;
					exponentSignAllowed = false;
				}
				else
				{
					sawSignificandDigit = true;
				}
				continue;
			}

			if (ch == '.' && !sawDecimalPoint && !sawExponent)
			{
				builder.Append(ch);
				sawDecimalPoint = true;
				continue;
			}

			if ((ch == 'e' || ch == 'E') && sawSignificandDigit && !sawExponent)
			{
				builder.Append('E');
				sawExponent = true;
				exponentSignAllowed = true;
				continue;
			}

			if ((ch == '+' || ch == '-') && exponentSignAllowed)
			{
				builder.Append(ch);
				exponentSignAllowed = false;
				continue;
			}

			break;
		}

		while (index < span.Length && char.IsWhiteSpace(span[index]))
		{
			index++;
		}

		consumedCount = (ushort)index;

		if (usedParentheses && !closedParentheses)
		{
			statusWord = 1;
			return false;
		}

		if (!sawSignificandDigit)
		{
			statusWord = 1;
			return false;
		}

		if (sawExponent && !sawExponentDigit)
		{
			statusWord = 2;
			return false;
		}

		normalized = builder.ToString();
		statusWord = 1;

		return true;
	}

	private static ushort FormatFmtrealScalar(ushort arg0, ushort arg1, ushort arg2, ushort arg3)
	{
		return arg0;
	}

	private static string FormatFmtrealNumericText(VmRuntimeState state, ushort sourceHandle, ushort controlWord0, ushort controlWord1)
	{
		double value = ReadReal64(state, sourceHandle);
		int scale = controlWord0 == FalseSentinel ? -1 : unchecked((short)controlWord0);
		byte flags = (byte)controlWord1;

		if (double.IsNaN(value))
		{
			return "NaN";
		}

		if (double.IsPositiveInfinity(value))
		{
			return "Infinity";
		}

		if (double.IsNegativeInfinity(value))
		{
			return "-Infinity";
		}

		if (scale >= 0)
		{
			string text = value.ToString($"F{Math.Clamp(scale, 0, 30)}", CultureInfo.InvariantCulture);
			// Native Cornerstone omits the leading zero before the decimal point
			if (text.StartsWith("0."))
				text = text[1..];
			else if (text.StartsWith("-0."))
				text = string.Concat("-", text.AsSpan(2));
			return text;
		}

		if ((flags & 0x08) != 0 || IsNearlyIntegral(value))
		{
			return Math.Round(value).ToString(CultureInfo.InvariantCulture);
		}

		return value.ToString("G15", CultureInfo.InvariantCulture);
	}

	private static bool IsNearlyIntegral(double value)
	{
		double rounded = Math.Round(value);
		return Math.Abs(value - rounded) < 1e-9;
	}
}

