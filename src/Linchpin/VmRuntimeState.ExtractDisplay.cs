/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Globalization;
using System.Text;

namespace Linchpin;

internal sealed partial class VmRuntimeState
{
	/// <summary>
	/// Executes the WPRINTV opcode, which prints a range of characters from a source vector
	/// into a display window, clipping to both the source bounds and the window's visible region.
	/// </summary>
	/// <param name="descriptorHandle">Handle to the display descriptor whose logical cursor position (word 0) determines where printing begins.</param>
	/// <param name="sourceHandle">Handle to the source vector containing the character data to print.</param>
	/// <param name="charCount">Number of characters to print.</param>
	/// <param name="sourceOffset">Byte offset within the source vector's payload at which to start reading characters.</param>
	/// <returns>The result of the cursor positioning (SETWIN) call after printing, or <c>FalseSentinel</c> on failure.</returns>
	public ushort ExecuteWprintv(ushort descriptorHandle, ushort sourceHandle, ushort charCount, ushort sourceOffset)
	{
		if (descriptorHandle == FalseSentinel || descriptorHandle == 0 || sourceHandle == FalseSentinel || sourceHandle == 0)
		{
			Trace($"WPRINTV descriptor=0x{descriptorHandle:X4} source=0x{sourceHandle:X4} N={charCount} O={sourceOffset} -> false");
			return FalseSentinel;
		}

		// Read descriptor and geometry once for clipping.
		ushort startCol = ReadAggregateWord(descriptorHandle, 0); // D[0] = logical column
		ushort geometryHandle = ReadAggregateWord(descriptorHandle, 3); // D[3]
		if (geometryHandle == FalseSentinel || geometryHandle == 0)
		{
			Trace($"WPRINTV descriptor=0x{descriptorHandle:X4} -> false (no geometry)");
			return FalseSentinel;
		}

		ushort srcLimit = ReadAggregateWord(sourceHandle, 0); // S[0] = source byte length
		int colOrigin = (short)ReadAggregateWord(geometryHandle, 0); // G[0]
		int colSpan = ReadAggregateWord(geometryHandle, 4); // G[4]

		// Logical column range.
		int start = (short)startCol;
		int endReq = start + charCount;

		// Source-limited end.
		int endSrc = (sourceOffset + charCount > srcLimit)
			? start + Math.Max(0, srcLimit - sourceOffset)
			: endReq;
		int end = Math.Min(endReq, endSrc);

		// Window limits.
		int winMin = colOrigin;
		int winMax = colOrigin + colSpan;

		// Left clip.
		int startClipped = start;
		int srcStart = sourceOffset;
		if (startClipped < winMin)
		{
			int adv = winMin - startClipped;
			srcStart += adv;
			startClipped = winMin;
		}

		// Right clip.
		int endClipped = Math.Min(end, winMax);

		// Visible count (hard-cap 100).
		int count = Math.Min(Math.Max(0, endClipped - startClipped), 100);

		// Store start_clipped into D[0] and call SETWIN.
		WriteAggregateWord(descriptorHandle, 0, (ushort)startClipped);
		ushort setwinResult = ExecuteSetwin(descriptorHandle);
		if (setwinResult == FalseSentinel)
		{
			// Restore D[0] to end_req so the descriptor still advances.
			WriteAggregateWord(descriptorHandle, 0, (ushort)endReq);
			Trace($"WPRINTV descriptor=0x{descriptorHandle:X4} source=0x{sourceHandle:X4} N={charCount} O={sourceOffset} -> false (SETWIN)");
			return FalseSentinel;
		}

		if (count > 0)
		{
			Host.PrintVector(this, sourceHandle, (ushort)count, (ushort)srcStart);
		}

		// After emit, store end_req into D[0] and call SETWIN again
		// so the descriptor reflects the logical cursor advance.
		WriteAggregateWord(descriptorHandle, 0, (ushort)endReq);
		ExecuteSetwin(descriptorHandle);

		if (count > 0)
		{
			var contentBytes = ReadAggregatePayloadBytes(sourceHandle, (ushort)count, (ushort)srcStart);
			string contentPreview = new string(contentBytes.ToArray().Select(b => b >= 0x20 && b < 0x7F ? (char)b : '.').ToArray());
			if (contentBytes.ToArray().Any(b => b < 0x20 || b >= 0x7F))
			{
				string hexPreview = string.Join(" ", contentBytes.ToArray().Select(b => $"{b:X2}"));
				Trace($"WPRINTV descriptor=0x{descriptorHandle:X4} source=0x{sourceHandle:X4} N={charCount} O={sourceOffset} count={count} hex=[{hexPreview}] -> 0x{setwinResult:X4} \"{contentPreview}\"");
			}
			else
			{
				Trace($"WPRINTV descriptor=0x{descriptorHandle:X4} source=0x{sourceHandle:X4} N={charCount} O={sourceOffset} count={count} -> 0x{setwinResult:X4} \"{contentPreview}\"");
			}
		}
		else
		{
			Trace($"WPRINTV descriptor=0x{descriptorHandle:X4} source=0x{sourceHandle:X4} N={charCount} O={sourceOffset} count={count} -> 0x{setwinResult:X4}");
		}
		return setwinResult;
	}

	/// <summary>
	/// Executes the SETWIN opcode, which positions the host console cursor according to
	/// the logical column and row stored in a display descriptor. Also updates the active
	/// display descriptor (system slot 0xD3) and display attribute (system slot 0xD5).
	/// </summary>
	/// <param name="descriptorHandle">Handle to the display descriptor containing the logical cursor position and display geometry.</param>
	/// <returns>The logical column value from the descriptor, or <c>FalseSentinel</c> if the descriptor is invalid or the position falls outside the window bounds.</returns>
	public ushort ExecuteSetwin(ushort descriptorHandle)
	{
		if (descriptorHandle == FalseSentinel || descriptorHandle == 0)
		{
			StoreModuleGlobal(0xD3, FalseSentinel);
			Trace($"SETWIN descriptor=0x{descriptorHandle:X4} -> false");
			return FalseSentinel;
		}

		ushort logicalColumn = ReadAggregateWord(descriptorHandle, 0);
		ushort logicalRow = ReadAggregateWord(descriptorHandle, 1);
		if (!TryResolveDisplayWindow(descriptorHandle, logicalColumn, logicalRow, out int hostRow, out int hostColumn, out _))
		{
			StoreModuleGlobal(0xD3, FalseSentinel);
			Trace($"SETWIN descriptor=0x{descriptorHandle:X4} col={logicalColumn} row={logicalRow} -> false");
			return FalseSentinel;
		}

		StoreModuleGlobal(0xD3, descriptorHandle);
		ushort attributeBits = ExtractBitField(descriptorHandle, 0x0802);
		StoreModuleGlobal(0xD5, attributeBits);
		Host.SetCursor(hostRow, hostColumn);
		Trace($"SETWIN descriptor=0x{descriptorHandle:X4} logical=(c={logicalColumn},r={logicalRow}) host=({hostRow},{hostColumn}) attr=0x{attributeBits:X4} -> 0x{logicalColumn:X4}");
		return logicalColumn;
	}

	/// <summary>
	/// Executes the LOOKUP opcode, which resolves a packed two-level key into a display descriptor
	/// handle. The key's low 7 bits index into a primary table, and the high byte indexes into
	/// a secondary table. If the secondary table entry is missing or contains the sentinel value
	/// 0x3FFF, a fallback far-call is made to a resolver procedure instead.
	/// </summary>
	/// <param name="returnProgramCounter">The program counter to resume at if a fallback far-call is initiated.</param>
	/// <param name="result">When the method returns <c>true</c>, contains the resolved descriptor handle.</param>
	/// <returns><c>true</c> if the lookup succeeded synchronously; <c>false</c> if a far-call fallback was initiated (the caller should not use <paramref name="result"/>).</returns>
	public bool TryExecuteLookup(int returnProgramCounter, out ushort result)
	{
		ushort packedKey = Pop();
		ushort rootHandle = LoadModuleGlobal(0xD8);
		if (rootHandle == FalseSentinel || rootHandle == 0)
		{
			throw new LinchpinException($"LOOKUP requires initialized system slot 0xD8 ({DescribeCurrentExecutionContext()}).");
		}

		int lowIndex = packedKey & 0x7F;
		if (lowIndex <= 0)
		{
			throw new LinchpinException($"LOOKUP received invalid packed key 0x{packedKey:X4}: low 7-bit index must be 1-based.");
		}

		int highIndex = (packedKey >> 8) & 0xFF;
		if (highIndex <= 0)
		{
			throw new LinchpinException($"LOOKUP received invalid packed key 0x{packedKey:X4}: high-byte index must be 1-based.");
		}

		ushort primaryTableHandle = ReadAggregateWord(rootHandle, 0);
		ushort secondaryTableHandle = ReadAggregateWord(primaryTableHandle, 2 * (lowIndex - 1));
		if (secondaryTableHandle == 0 || secondaryTableHandle == FalseSentinel)
		{
			ushort resolverSelector = ReadAggregateWord(rootHandle, 3);
			Trace($"LOOKUP key=0x{packedKey:X4} root=0x{rootHandle:X4} table0=0x{primaryTableHandle:X4} table1=0x{secondaryTableHandle:X4} -> fallback selector 0x{resolverSelector:X4}");
			Push(packedKey);
			EnterFarCall(resolverSelector, 1, returnProgramCounter);
			result = 0;
			return false;
		}

		ushort relativeDescriptor = (ushort)(ReadAggregateWord(secondaryTableHandle, highIndex - 1) & 0x3FFF);
		if (relativeDescriptor != 0x3FFF)
		{
			ushort descriptorBaseHandle = ReadAggregateWord(rootHandle, 2);
			result = unchecked((ushort)(descriptorBaseHandle + relativeDescriptor));
			Trace($"LOOKUP key=0x{packedKey:X4} root=0x{rootHandle:X4} table0=0x{primaryTableHandle:X4} table1=0x{secondaryTableHandle:X4} rel=0x{relativeDescriptor:X4} -> 0x{result:X4}");
			return true;
		}

		ushort fallbackSelector = ReadAggregateWord(rootHandle, 3);
		Trace($"LOOKUP key=0x{packedKey:X4} root=0x{rootHandle:X4} -> fallback selector 0x{fallbackSelector:X4}");
		Push(packedKey);
		EnterFarCall(fallbackSelector, 1, returnProgramCounter);
		result = 0;
		return false;
	}

	/// <summary>
	/// Executes the EXTRACT opcode, which performs a complex multi-mode field extraction from
	/// an aggregate data structure. Pops eight arguments from the evaluation stack: <em>base</em> handle,
	/// <em>span</em> (word count), <em>destination</em> handle, <em>tag</em> selector, <em>skip</em> count,
	/// <em>offset</em>, <em>guard</em>, and <em>mode</em>. Depending on the combination of arguments,
	/// the extraction may walk tagged records, resolve typed fields, copy raw bytes, or return
	/// component counts.
	/// </summary>
	/// <returns>The extracted field length, component count, or data value; or <c>FalseSentinel</c> on failure.</returns>
	public ushort ExecuteExtract()
	{
		if (evaluationStack.Count < 8)
		{
			Trace($"EXTRACT -> false (stack depth {evaluationStack.Count})");
			return FalseSentinel;
		}

		ushort mode = Pop();
		ushort nonZeroGuard = Pop();
		ushort initialWordOffset = Pop();
		ushort recordSkipCount = Pop();
		ushort recordTagSelector = Pop();
		ushort destinationHandle = Pop();
		ushort availableSpan = Pop();
		ushort baseHandle = Pop();

		if (!IsUsableAggregateHandle(baseHandle))
		{
			Trace($"EXTRACT base=0x{baseHandle:X4} span=0x{availableSpan:X4} dst=0x{destinationHandle:X4} tag=0x{recordTagSelector:X4} skip=0x{recordSkipCount:X4} off=0x{initialWordOffset:X4} guard=0x{nonZeroGuard:X4} mode=0x{mode:X4} -> false");
			return FalseSentinel;
		}

		// --- Step 1: Offset adjustment ---
		// When recordSkipCount == FALSE, recordTagSelector is used directly as
		// a word offset into the base aggregate.
		ushort wordOffset;
		if (recordSkipCount != FalseSentinel)
		{
			// skip != FALSE: call record-skipping logic to compute offset
			wordOffset = ComputeExtractSkipOffset(baseHandle, availableSpan, recordTagSelector, recordSkipCount);
		}
		else
		{
			// skip == FALSE: tag IS the word offset
			wordOffset = recordTagSelector;
		}

		ushort effectiveBase = unchecked((ushort)(baseHandle + wordOffset));
		ushort effectiveSpan = unchecked((ushort)(availableSpan - wordOffset));

		// --- Step 2: Mode/offset dispatch ---
		if (initialWordOffset != FalseSentinel)
		{
			// Complex scanning: initialWordOffset specifies where to look within the adjusted data.
			// Delegate to the existing selection + extraction logic.
			return ExecuteExtractWithOffset(effectiveBase, effectiveSpan, destinationHandle, initialWordOffset, nonZeroGuard, mode);
		}

		if (mode != FalseSentinel)
		{
			// mode != FALSE: use the field walker to locate and extract a field.
			return ExecuteExtractFieldWalker(effectiveBase, effectiveSpan, destinationHandle, mode, nonZeroGuard);
		}

		// mode == FALSE, off == FALSE: direct extraction from adjusted base.
		if (nonZeroGuard != FalseSentinel)
		{
			// Guard-based direct extraction:
			// 1. Probe for nonzero bytes in first `guard` bytes
			// 2. Check destination capacity
			// 3. Copy guard bytes from source to destination
			if (!AggregateRawByteSpanContainsNonZero(effectiveBase, 0, nonZeroGuard))
			{
				Trace($"EXTRACT guard direct: effectiveBase=0x{effectiveBase:X4} guard=0x{nonZeroGuard:X4} -> false (all zero)");
				return FalseSentinel;
			}

			ushort destCapacity = ReadAggregateWord(destinationHandle, 0);
			if (destCapacity >= nonZeroGuard && CanWriteExtractDestination(destinationHandle, nonZeroGuard))
			{
				CopyAggregateRawBytesToPayload(effectiveBase, 0, destinationHandle, 0, nonZeroGuard);
			}

			Trace($"EXTRACT guard direct: effectiveBase=0x{effectiveBase:X4} guard=0x{nonZeroGuard:X4} -> 0x{nonZeroGuard:X4}");
			return nonZeroGuard;
		}

		// guard == FALSE: read packed field at adjusted base and extract.
		return ExecuteExtractLeadingPackedField(effectiveBase, effectiveSpan, destinationHandle);
	}

	/// <summary>
	/// Computes the byte-aligned word offset for EXTRACT when <paramref name="recordSkipCount"/> is not FALSE.
	/// Starting at the word offset given by <paramref name="recordSkipCount"/>, walks past
	/// <paramref name="recordTagSelector"/> typed-field records and returns the resulting word offset.
	/// </summary>
	private ushort ComputeExtractSkipOffset(ushort baseHandle, ushort availableSpan, ushort recordTagSelector, ushort recordSkipCount)
	{
		int currentWordOffset = recordSkipCount;
		int recordsToWalk = recordTagSelector;

		for (int i = 0; i < recordsToWalk; i++)
		{
			int remainingWords = availableSpan - currentWordOffset;
			if (!TryMeasureExtractTypedField(baseHandle, currentWordOffset, remainingWords, out ExtractRecordSelection field))
			{
				break;
			}
			currentWordOffset += field.WordLength;
		}

		return (ushort)currentWordOffset;
	}

	/// <summary>
	/// Handles the EXTRACT case where <paramref name="initialWordOffset"/> is not FALSE.
	/// Attempts to select a record at the given offset, falling back to typed-field measurement
	/// if record selection fails, then delegates to <see cref="ApplyExtractSelection"/>.
	/// </summary>
	private ushort ExecuteExtractWithOffset(ushort effectiveBase, ushort effectiveSpan, ushort destinationHandle, ushort initialWordOffset, ushort nonZeroGuard, ushort mode)
	{
		bool usedTypedFieldFallback = false;
		if (!TrySelectExtractRecord(effectiveBase, effectiveSpan, initialWordOffset, FalseSentinel, FalseSentinel, out ExtractRecordSelection selection))
		{
			if (!TrySelectExtractTypedFieldFallback(effectiveBase, effectiveSpan, initialWordOffset, FalseSentinel, FalseSentinel, out selection))
			{
				Trace($"EXTRACT withOffset: effectiveBase=0x{effectiveBase:X4} off=0x{initialWordOffset:X4} -> false");
				return FalseSentinel;
			}

			usedTypedFieldFallback = true;
		}

		return ApplyExtractSelection(effectiveBase, effectiveSpan, destinationHandle, selection, nonZeroGuard, mode, usedTypedFieldFallback);
	}

	/// <summary>
	/// Handles the EXTRACT field-walker case where <paramref name="mode"/> is not FALSE and
	/// no explicit word offset is provided. Measures the first record or typed field at the
	/// effective base and then applies the selection.
	/// </summary>
	private ushort ExecuteExtractFieldWalker(ushort effectiveBase, ushort effectiveSpan, ushort destinationHandle, ushort mode, ushort nonZeroGuard)
	{
		bool usedTypedFieldFallback = false;
		if (!TrySelectExtractRecord(effectiveBase, effectiveSpan, FalseSentinel, FalseSentinel, FalseSentinel, out ExtractRecordSelection selection))
		{
			if (!TryMeasureExtractTypedField(effectiveBase, 0, effectiveSpan, out selection))
			{
				Trace($"EXTRACT fieldWalker: effectiveBase=0x{effectiveBase:X4} mode=0x{mode:X4} -> false");
				return FalseSentinel;
			}

			usedTypedFieldFallback = true;
		}

		return ApplyExtractSelection(effectiveBase, effectiveSpan, destinationHandle, selection, nonZeroGuard, mode, usedTypedFieldFallback);
	}

	/// <summary>
	/// Handles the EXTRACT case where both guard and mode are FALSE. Reads a leading packed field
	/// at the adjusted base: the low byte of the first word gives the field length, and the data
	/// begins at the next word. Copies the field data into the destination if it fits.
	/// </summary>
	private ushort ExecuteExtractLeadingPackedField(ushort effectiveBase, ushort effectiveSpan, ushort destinationHandle)
	{
		// Read the marker byte (low byte of word[0]) which gives the field length.
		ushort fieldLen = (ushort)(ReadAggregateWord(effectiveBase, 0) & 0xFF);

		// Advance past the marker word to the start of the field data.
		ushort dataBase = unchecked((ushort)(effectiveBase + 1));

		if (fieldLen == 0)
		{
			return FalseSentinel;
		}

		// Copy fieldLen bytes from dataBase to destination
		ushort destCapacity = ReadAggregateWord(destinationHandle, 0);
		if (destCapacity >= fieldLen && CanWriteExtractDestination(destinationHandle, fieldLen))
		{
			CopyAggregateRawBytesToPayload(dataBase, 0, destinationHandle, 0, fieldLen);
		}

		Trace($"EXTRACT leadingPacked: effectiveBase=0x{effectiveBase:X4} dataBase=0x{dataBase:X4} fieldLen={fieldLen} -> 0x{fieldLen:X4}");
		return fieldLen;
	}

	/// <summary>
	/// Applies the result of a record or typed-field selection during EXTRACT. Depending on
	/// <paramref name="mode"/>, copies the selected payload to the destination, returns the
	/// component count, or resolves a subfield within the selection.
	/// </summary>
	private ushort ApplyExtractSelection(ushort baseHandle, ushort availableSpan, ushort destinationHandle, ExtractRecordSelection selection, ushort nonZeroGuard, ushort mode, bool usedTypedFieldFallback)
	{
		ushort result;

		if (usedTypedFieldFallback)
		{
			if (mode == FalseSentinel)
			{
				if (nonZeroGuard != FalseSentinel
					&& !AggregateByteSpanContainsNonZero(baseHandle, selection.PayloadByteOffset, selection.PayloadByteLength))
				{
					return FalseSentinel;
				}

				result = checked((ushort)selection.PayloadByteLength);
				ushort rawByteLength = checked((ushort)(result + 2));
				if (CanWriteExtractDestination(destinationHandle, rawByteLength))
				{
					CopyAggregateBytes(baseHandle, selection.WordOffset * 2, destinationHandle, 0, rawByteLength);
				}
			}
			else if (mode == 0 || mode == 0xFFFF)
			{
				result = selection.ComponentCount;
			}
			else
			{
				if (!TryResolveExtractTypedSubfield(baseHandle, selection, mode, out ExtractSubfieldSelection subfield))
				{
					return FalseSentinel;
				}

				if (nonZeroGuard != FalseSentinel
					&& !AggregateByteSpanContainsNonZero(baseHandle, subfield.DataByteOffset, subfield.ByteLength))
				{
					return FalseSentinel;
				}

				result = subfield.ByteLength;
				if (CanWriteExtractDestination(destinationHandle, result))
				{
					CopyAggregateBytes(baseHandle, subfield.DataByteOffset, destinationHandle, 0, result);
				}
			}
		}
		else if (mode == FalseSentinel)
		{
			if (nonZeroGuard != FalseSentinel
				&& !AggregateByteSpanContainsNonZero(baseHandle, selection.PayloadByteOffset, selection.PayloadByteLength))
			{
				return FalseSentinel;
			}

			result = checked((ushort)selection.PayloadByteLength);
			if (CanWriteExtractDestination(destinationHandle, result))
			{
				CopyAggregateBytes(baseHandle, selection.PayloadByteOffset, destinationHandle, 0, result);
			}
		}
		else if (mode == 0 || mode == 0xFFFF)
		{
			result = selection.ComponentCount;
		}
		else
		{
			if (!TryResolveExtractSubfield(baseHandle, selection, mode, out ExtractSubfieldSelection subfield))
			{
				return FalseSentinel;
			}

			result = subfield.ByteLength;
			if (CanWriteExtractDestination(destinationHandle, result))
			{
				CopyAggregateBytes(baseHandle, subfield.DataByteOffset, destinationHandle, 0, result);
			}
		}

		Trace($"EXTRACT extract: base=0x{baseHandle:X4} selOff=0x{selection.WordOffset:X4} selTag=0x{selection.Tag:X2} payloadBytes=0x{selection.PayloadByteLength:X4} mode=0x{mode:X4} -> 0x{result:X4}");
		return result;
	}

	private bool TrySelectExtractRecord(ushort baseHandle, ushort availableSpan, ushort initialWordOffset, ushort recordTagSelector, ushort recordSkipCount, out ExtractRecordSelection selection)
	{
		selection = default;

		int currentWordOffset = initialWordOffset == FalseSentinel ? 0 : initialWordOffset;
		int remainingWords = availableSpan;
		if (currentWordOffset < 0 || remainingWords <= 0)
		{
			return false;
		}

		if (recordSkipCount != FalseSentinel)
		{
			for (int index = 0; index < recordSkipCount; index++)
			{
				if (!TryMeasureExtractRecord(baseHandle, currentWordOffset, remainingWords, out ExtractRecordSelection skipped))
				{
					return false;
				}

				currentWordOffset += skipped.WordLength;
				remainingWords -= skipped.WordLength;
			}
		}

		if (recordTagSelector != FalseSentinel)
		{
			byte requestedTag = (byte)(recordTagSelector & 0xFF);
			while (remainingWords > 0)
			{
				if (!TryMeasureExtractRecord(baseHandle, currentWordOffset, remainingWords, out ExtractRecordSelection candidate))
				{
					return false;
				}

				if (candidate.Tag == requestedTag)
				{
					selection = candidate;
					return true;
				}

				currentWordOffset += candidate.WordLength;
				remainingWords -= candidate.WordLength;
			}

			return false;
		}

		return TryMeasureExtractRecord(baseHandle, currentWordOffset, remainingWords, out selection);
	}

	private bool TrySelectExtractTypedFieldFallback(ushort baseHandle, ushort availableSpan, ushort initialWordOffset, ushort recordTagSelector, ushort recordSkipCount, out ExtractRecordSelection selection)
	{
		selection = default;
		if (initialWordOffset != FalseSentinel || recordTagSelector == FalseSentinel)
		{
			return false;
		}

		if (recordSkipCount != FalseSentinel)
		{
			int currentWordOffset = recordSkipCount;
			if (currentWordOffset < 0 || currentWordOffset >= availableSpan)
			{
				return false;
			}

			if (!TryMeasureExtractTypedField(baseHandle, currentWordOffset, availableSpan - currentWordOffset, out ExtractRecordSelection skippedField))
			{
				return false;
			}

			currentWordOffset += skippedField.WordLength;
			int remainingWords = availableSpan - currentWordOffset;

			for (int ordinal = 1; ordinal <= recordTagSelector; ordinal++)
			{
				if (!TryMeasureExtractTypedField(baseHandle, currentWordOffset, remainingWords, out ExtractRecordSelection candidate))
				{
					return false;
				}

				if (ordinal == recordTagSelector)
				{
					selection = candidate;
					return true;
				}

				currentWordOffset += candidate.WordLength;
				remainingWords -= candidate.WordLength;
			}

			return false;
		}

		int wordOffset = recordTagSelector;
		if (wordOffset < 0 || wordOffset >= availableSpan)
		{
			return false;
		}

		return TryMeasureExtractTypedField(baseHandle, wordOffset, availableSpan - wordOffset, out selection);
	}

	private bool TryMeasureExtractRecord(ushort baseHandle, int wordOffset, int remainingWords, out ExtractRecordSelection selection)
	{
		selection = default;
		if (remainingWords <= 0 || wordOffset < 0)
		{
			return false;
		}

		ushort headerWord = ReadAggregateWord(baseHandle, wordOffset);
		byte tag = (byte)(headerWord & 0xFF);
		byte componentCount = (byte)(headerWord >> 8);
		int payloadByteOffset = checked(wordOffset * 2 + 2);

		if (!TryMeasureExtractPayload(baseHandle, payloadByteOffset, remainingWords - 1, componentCount, out int payloadByteLength, out int payloadWordLength))
		{
			return false;
		}

		selection = new ExtractRecordSelection(
			wordOffset,
			1 + payloadWordLength,
			payloadByteOffset,
			payloadByteLength,
			tag,
			componentCount);
		return selection.WordLength <= remainingWords;
	}

	private bool TryMeasureExtractTypedField(ushort baseHandle, int wordOffset, int remainingWords, out ExtractRecordSelection selection)
	{
		selection = default;
		if (remainingWords <= 0 || wordOffset < 0)
		{
			return false;
		}

		ushort headerWord = ReadAggregateWord(baseHandle, wordOffset);
		byte fieldKind = (byte)(headerWord & 0x00FF);
		byte componentCount = (byte)(headerWord >> 8);
		int payloadByteOffset = checked(wordOffset * 2);
		int fieldWordLength;
		int payloadByteLength;

		if (headerWord == 0)
		{
			fieldWordLength = 1;
			payloadByteLength = 0;
		}
		else if (fieldKind == 0)
		{
			if (remainingWords < 2)
			{
				return false;
			}

			fieldWordLength = ReadAggregateWord(baseHandle, wordOffset + 1);
			if (fieldWordLength <= 0)
			{
				return false;
			}

			payloadByteLength = (fieldWordLength - 1) * 2;
		}
		else
		{
			int componentWordLength = (fieldKind + 1) / 2;
			fieldWordLength = 1 + componentCount * componentWordLength;
			payloadByteLength = componentCount * fieldKind;
		}

		if (fieldWordLength > remainingWords)
		{
			return false;
		}

		selection = new ExtractRecordSelection(
			wordOffset,
			fieldWordLength,
			payloadByteOffset,
			payloadByteLength,
			fieldKind,
			componentCount);
		return true;
	}

	private string FormatAggregateWordPreview(ushort handle, int startWordOffset, int wordCount)
	{
		if (wordCount <= 0)
		{
			return "{}";
		}

		StringBuilder builder = new();
		builder.Append('{');
		for (int index = 0; index < wordCount; index++)
		{
			if (index != 0)
			{
				builder.Append(", ");
			}

			int wordOffset = startWordOffset + index;
			builder.Append('[');
			builder.Append(wordOffset.ToString("X4", CultureInfo.InvariantCulture));
			builder.Append("]=0x");
			builder.Append(ReadAggregateWord(handle, wordOffset).ToString("X4", CultureInfo.InvariantCulture));
		}

		builder.Append('}');
		return builder.ToString();
	}

	private bool TryMeasureExtractPayload(ushort baseHandle, int payloadByteOffset, int remainingPayloadWords, byte componentCount, out int payloadByteLength, out int payloadWordLength)
	{
		payloadByteLength = 0;
		payloadWordLength = 0;

		if (remainingPayloadWords < 0)
		{
			return false;
		}

		if (componentCount == 0)
		{
			return TryMeasureExtractPackedField(baseHandle, payloadByteOffset, remainingPayloadWords, out payloadByteLength, out payloadWordLength);
		}

		int byteCursor = payloadByteOffset;
		int wordsUsed = 0;
		int bytesUsed = 0;
		for (int index = 0; index < componentCount; index++)
		{
			if (!TryMeasureExtractPackedField(baseHandle, byteCursor, remainingPayloadWords - wordsUsed, out int fieldByteLength, out int fieldWordLength))
			{
				return false;
			}

			bytesUsed += fieldByteLength;
			wordsUsed += fieldWordLength;
			byteCursor += fieldWordLength * 2;
		}

		payloadByteLength = bytesUsed;
		payloadWordLength = wordsUsed;
		return true;
	}

	private bool TryMeasureExtractPackedField(ushort baseHandle, int fieldByteOffset, int remainingFieldWords, out int fieldByteLength, out int fieldWordLength)
	{
		fieldByteLength = 0;
		fieldWordLength = 0;
		if (remainingFieldWords <= 0)
		{
			return false;
		}

		int logicalByteLength = ReadAggregateByte(baseHandle, fieldByteOffset);
		if ((logicalByteLength & 0x80) != 0)
		{
			fieldByteLength = 8;
			fieldWordLength = 4;
		}
		else
		{
			fieldByteLength = logicalByteLength + 1;
			fieldWordLength = (fieldByteLength + 1) / 2;
		}

		return fieldWordLength <= remainingFieldWords;
	}

	private bool TryResolveExtractSubfield(ushort baseHandle, ExtractRecordSelection selection, ushort mode, out ExtractSubfieldSelection subfield)
	{
		subfield = default;
		if (selection.ComponentCount == 0 || mode > selection.ComponentCount)
		{
			return false;
		}

		int byteCursor = selection.PayloadByteOffset;
		int remainingWords = selection.WordLength - 1;
		for (int index = 1; index <= selection.ComponentCount; index++)
		{
			if (!TryMeasureExtractPackedField(baseHandle, byteCursor, remainingWords, out int fieldByteLength, out int fieldWordLength))
			{
				return false;
			}

			if (index == mode)
			{
				subfield = new ExtractSubfieldSelection(
					checked((byte)(fieldByteLength - 1)),
					byteCursor + 1,
					fieldByteLength,
					fieldWordLength);
				return true;
			}

			byteCursor += fieldWordLength * 2;
			remainingWords -= fieldWordLength;
		}

		return false;
	}

	private bool TryResolveExtractTypedSubfield(ushort baseHandle, ExtractRecordSelection selection, ushort mode, out ExtractSubfieldSelection subfield)
	{
		subfield = default;
		if (selection.ComponentCount == 0 || mode == 0 || mode > selection.ComponentCount)
		{
			return false;
		}

		if (selection.Tag == 0)
		{
			int byteCursor = selection.PayloadByteOffset;
			int remainingWords = selection.WordLength - 2;
			for (int index = 1; index <= mode; index++)
			{
				if (!TryMeasureExtractPackedField(baseHandle, byteCursor, remainingWords, out int fieldByteLength, out int fieldWordLength))
				{
					return false;
				}

				if (index == mode)
				{
					subfield = new ExtractSubfieldSelection(
						checked((byte)(fieldByteLength - 1)),
						byteCursor + 1,
						fieldByteLength,
						fieldWordLength);
					return true;
				}

				byteCursor += fieldWordLength * 2;
				remainingWords -= fieldWordLength;
			}

			return false;
		}

		int componentWordLength = (selection.Tag + 1) / 2;
		int componentByteOffset = selection.PayloadByteOffset + (mode - 1) * componentWordLength * 2;
		subfield = new ExtractSubfieldSelection(
			selection.Tag,
			componentByteOffset,
			selection.Tag,
			componentWordLength);
		return true;
	}

	private bool AggregateByteSpanContainsNonZero(ushort handle, int byteOffset, int byteLength)
	{
		for (int index = 0; index < byteLength; index++)
		{
			if (ReadAggregateByte(handle, byteOffset + index) != 0)
			{
				return true;
			}
		}

		return false;
	}

	private bool AggregateRawByteSpanContainsNonZero(ushort handle, int rawByteOffset, int byteLength)
	{
		for (int index = 0; index < byteLength; index++)
		{
			if (ReadAggregateRawByte(handle, rawByteOffset + index) != 0)
			{
				return true;
			}
		}

		return false;
	}

	private bool CanWriteExtractDestination(ushort destinationHandle, ushort byteLength)
	{
		return IsUsableAggregateHandle(destinationHandle)
			&& ReadAggregateWord(destinationHandle, 0) >= byteLength;
	}

	private static bool IsUsableAggregateHandle(ushort handle)
	{
		return handle != 0 && handle != FalseSentinel;
	}

	private bool TryResolveDisplayWindow(ushort descriptorHandle, ushort logicalColumn, ushort logicalRow, out int hostRow, out int hostColumn, out int remainingWindowWidth)
	{
		hostRow = 0;
		hostColumn = 0;
		remainingWindowWidth = 0;

		ushort geometryHandle = ReadAggregateWord(descriptorHandle, 3);
		if (geometryHandle == FalseSentinel || geometryHandle == 0)
		{
			return false;
		}

		// Descriptor word 0 = column, word 1 = row (confirmed by runtime values:
		// D[0] reaches 63+ while row extent is small like 1).
		// Geometry fields follow the same column-first layout:
		//   G[0]=column origin  G[1]=row origin
		//   G[2]=column phys    G[3]=row phys
		//   G[4]=column extent  G[5]=row extent
		int col = (short)logicalColumn;
		int row = (short)logicalRow;
		int originCol = (short)ReadAggregateWord(geometryHandle, 0);
		int originRow = (short)ReadAggregateWord(geometryHandle, 1);
		int physCol = (short)ReadAggregateWord(geometryHandle, 2);
		int physRow = (short)ReadAggregateWord(geometryHandle, 3);
		int colExtent = ReadAggregateWord(geometryHandle, 4);
		int rowExtent = ReadAggregateWord(geometryHandle, 5);

		int deltaCol = col - originCol;
		int deltaRow = row - originRow;
		if (deltaCol < 0 || deltaRow < 0 || deltaCol >= colExtent || deltaRow >= rowExtent)
		{
			return false;
		}

		hostColumn = physCol + deltaCol;
		hostRow = physRow + deltaRow;
		remainingWindowWidth = Math.Max(0, colExtent - deltaCol);
		return hostRow is >= 0 and < 25 && hostColumn is >= 0 and < 80;
	}
}
