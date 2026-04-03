/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Linchpin;

internal sealed partial class VmRuntimeState
{
	/// <summary>
	/// Reads a 16-bit word from an aggregate (vector or tuple) in VM RAM.
	/// </summary>
	/// <param name="handle">The VM handle identifying the aggregate's base address.</param>
	/// <param name="wordIndex">Zero-based word index within the aggregate.</param>
	public ushort ReadAggregateWord(ushort handle, int wordIndex)
	{
		if (IsFalseSentinelAccess(handle, "ReadAggregateWord", wordIndex))
			return 0;

		int byteOffset = ResolveVmWordByteOffset(handle, wordIndex);
		if (byteOffset + 1 >= RamBytes.Length)
		{
			throw new LinchpinException($"Aggregate word read at handle 0x{handle:X4}, index {wordIndex} runs past initial RAM ({DescribeCurrentExecutionContext()}{DescribeLocalVectorPreview()}).");
		}

		return (ushort)(RamBytes[byteOffset] | (RamBytes[byteOffset + 1] << 8));
	}

	public int ResolveDynamicWordReadIndex(ushort handle, ushort wordIndex)
	{
		if (UsesOneBasedDynamicWordIndex(handle) && wordIndex > 0)
		{
			return wordIndex - 1;
		}

		return wordIndex;
	}

	private static bool UsesOneBasedDynamicWordIndex(ushort handle)
	{
		return handle != 0 && handle != FalseSentinel;
	}

	/// <summary>
	/// Writes a 16-bit word into an aggregate in VM RAM.
	/// </summary>
	public void WriteAggregateWord(ushort handle, int wordIndex, ushort value)
	{
		if (IsFalseSentinelAccess(handle, "WriteAggregateWord", wordIndex))
			return;

		int byteOffset = ResolveVmWordByteOffset(handle, wordIndex);
		if (byteOffset + 1 >= RamBytes.Length)
		{
			throw new LinchpinException($"Aggregate word write at handle 0x{handle:X4}, index {wordIndex} runs past initial RAM.");
		}

		RamBytes[byteOffset] = (byte)(value & 0xFF);
		RamBytes[byteOffset + 1] = (byte)(value >> 8);
	}

	/// <summary>
	/// Reads a single payload byte from an aggregate. The payload starts at word offset 1
	/// (i.e. byte offset 2 from the aggregate base), so byte index 0 is the first payload byte.
	/// </summary>
	public ushort ReadAggregateByte(ushort handle, int byteIndex)
	{
		if (IsFalseSentinelAccess(handle, "ReadAggregateByte", byteIndex))
			return 0;

		int offset = ResolveVmPayloadByteOffset(handle, byteIndex);
		if (offset < 0 || offset >= RamBytes.Length)
		{
			throw new LinchpinException($"Aggregate byte read at handle 0x{handle:X4}, index {byteIndex} runs past initial RAM.");
		}

		return RamBytes[offset];
	}

	/// <summary>
	/// Reads a raw byte from an aggregate at an absolute byte offset from the aggregate base,
	/// without the payload-skip adjustment used by <see cref="ReadAggregateByte"/>.
	/// </summary>
	public ushort ReadAggregateRawByte(ushort handle, int rawByteOffset)
	{
		if (IsFalseSentinelAccess(handle, "ReadAggregateRawByte", rawByteOffset))
			return 0;

		int offset = ResolveVmRawByteOffset(handle, rawByteOffset);
		if (offset < 0 || offset >= RamBytes.Length)
		{
			throw new LinchpinException($"Aggregate raw byte read at handle 0x{handle:X4}, offset {rawByteOffset} runs past initial RAM.");
		}

		return RamBytes[offset];
	}

	public void WriteAggregateByte(ushort handle, int byteIndex, byte value)
	{
		if (IsFalseSentinelAccess(handle, "WriteAggregateByte", byteIndex))
			return;

		int offset = ResolveVmPayloadByteOffset(handle, byteIndex);
		if (offset < 0 || offset >= RamBytes.Length)
		{
			throw new LinchpinException($"Aggregate byte write at handle 0x{handle:X4}, index {byteIndex} runs past initial RAM.");
		}

		RamBytes[offset] = value;
	}

	public void WriteAggregateRawByte(ushort handle, int rawByteOffset, byte value)
	{
		if (IsFalseSentinelAccess(handle, "WriteAggregateRawByte", rawByteOffset))
			return;

		int offset = ResolveVmRawByteOffset(handle, rawByteOffset);
		if (offset < 0 || offset >= RamBytes.Length)
		{
			throw new LinchpinException($"Aggregate raw byte write at handle 0x{handle:X4}, offset {rawByteOffset} runs past initial RAM.");
		}

		RamBytes[offset] = value;
	}

	public void FillAggregateWords(ushort handle, ushort count, ushort value)
	{
		for (int index = 0; index < count; index++)
		{
			WriteAggregateWord(handle, index, value);
		}
	}

	public void FillAggregateBytes(ushort handle, ushort count, byte value)
	{
		int boundedCount = ClampManagedPayloadByteCount(handle, 0, count);
		for (int index = 0; index < boundedCount; index++)
		{
			WriteAggregateByte(handle, index, value);
		}
	}

	/// <summary>
	/// Fills payload bytes without clamping to the declared word count.
	/// MME does not restrict VECSETB/VECCPYB to the payload boundary, so
	/// bytes that overflow the declared size spill into adjacent memory.
	/// </summary>
	public void FillAggregateBytesUnclamped(ushort handle, ushort count, byte value)
	{
		for (int index = 0; index < count; index++)
		{
			WriteAggregateByte(handle, index, value);
		}
	}

	public void CopyAggregateWords(ushort sourceHandle, ushort destinationHandle, ushort count)
	{
		ushort[] buffer = new ushort[count];
		for (int index = 0; index < count; index++)
		{
			buffer[index] = ReadAggregateWord(sourceHandle, index);
		}

		for (int index = 0; index < count; index++)
		{
			WriteAggregateWord(destinationHandle, index, buffer[index]);
		}
	}

	public void CopyAggregateBytes(ushort sourceHandle, int sourceOffset, ushort destinationHandle, int destinationOffset, ushort count)
	{
		int boundedCount = ClampManagedPayloadByteCount(sourceHandle, sourceOffset, count);
		boundedCount = ClampManagedPayloadByteCount(destinationHandle, destinationOffset, boundedCount);

		byte[] buffer = new byte[boundedCount];
		for (int index = 0; index < boundedCount; index++)
		{
			buffer[index] = (byte)ReadAggregateByte(sourceHandle, sourceOffset + index);
		}

		for (int index = 0; index < boundedCount; index++)
		{
			WriteAggregateByte(destinationHandle, destinationOffset + index, buffer[index]);
		}
	}

	/// <summary>
	/// Copies payload bytes without clamping to the declared word count.
	/// MME does not restrict VECSETB/VECCPYB to the payload boundary, so
	/// bytes that overflow the declared size spill into adjacent memory.
	/// </summary>
	public void CopyAggregateBytesUnclamped(ushort sourceHandle, int sourceOffset, ushort destinationHandle, int destinationOffset, ushort count)
	{
		byte[] buffer = new byte[count];
		for (int index = 0; index < count; index++)
		{
			buffer[index] = (byte)ReadAggregateByte(sourceHandle, sourceOffset + index);
		}

		for (int index = 0; index < count; index++)
		{
			WriteAggregateByte(destinationHandle, destinationOffset + index, buffer[index]);
		}
	}

	public void CopyAggregateRawBytesToPayload(ushort sourceHandle, int sourceOffset, ushort destinationHandle, int destinationOffset, ushort count)
	{
		int boundedCount = ClampManagedPayloadByteCount(destinationHandle, destinationOffset, count);

		byte[] buffer = new byte[boundedCount];
		for (int index = 0; index < boundedCount; index++)
		{
			buffer[index] = (byte)ReadAggregateRawByte(sourceHandle, sourceOffset + index);
		}

		for (int index = 0; index < boundedCount; index++)
		{
			WriteAggregateByte(destinationHandle, destinationOffset + index, buffer[index]);
		}
	}

	private int ClampManagedPayloadByteCount(ushort handle, int startOffset, int requestedCount)
	{
		if (!managedAggregateWordCounts.TryGetValue(handle, out ushort wordCount))
		{
			return requestedCount;
		}

		int payloadBytes = Math.Max(0, (wordCount - 1) * 2);
		int remainingBytes = Math.Max(0, payloadBytes - Math.Max(0, startOffset));
		return Math.Min(requestedCount, remainingBytes);
	}

	public ReadOnlySpan<byte> ReadAggregatePayloadBytes(ushort handle, ushort byteLength, ushort startOffset)
	{
		int payloadOffset = ResolveVmPayloadByteOffset(handle, startOffset);
		if (payloadOffset < 0 || payloadOffset > RamBytes.Length)
		{
			throw new LinchpinException($"ReadAggregatePayloadBytes out of bounds: handle=0x{handle:X4} startOffset={startOffset} byteLength={byteLength} payloadOffset={payloadOffset} ramSize={RamBytes.Length} ({DescribeCurrentExecutionContext()})");
		}
		int length = Math.Min(byteLength, (ushort)Math.Max(0, RamBytes.Length - payloadOffset));
		return RamBytes.AsSpan(payloadOffset, length);
	}

	private int ResolveVmWordByteOffset(ushort handle, int wordIndex)
	{
		return checked(ResolveVmBaseByteOffset(handle) + wordIndex * 2);
	}

	private int ResolveVmPayloadByteOffset(ushort handle, int byteIndex)
	{
		return checked(ResolveVmBaseByteOffset(handle) + 2 + byteIndex);
	}

	private int ResolveVmRawByteOffset(ushort handle, int rawByteOffset)
	{
		return checked(ResolveVmBaseByteOffset(handle) + rawByteOffset);
	}

	/// <summary>
	/// Returns true if handle is the FALSE sentinel (0x8001).
	/// The original interpreter's load subroutine returns 0 for reads on a FALSE handle
	/// (its index-to-byte-offset path short-circuits when the result would read from
	/// the sentinel address). Callers should return 0 when this returns true.
	/// </summary>

	private bool IsFalseSentinelAccess(ushort handle, string operation, int index)
	{
		if (handle != FalseSentinel)
			return false;

		Trace($"SENTINEL {operation} on FalseSentinel handle (0x{FalseSentinel:X4}) at index {index} " +
			$"({DescribeCurrentExecutionContext()}).");
		return true;
	}

	private static int ResolveVmBaseByteOffset(ushort handle)
	{
		int segmentBase = (handle & 0x8000) != 0 ? 0x10000 : 0;
		int wordAddress = handle & 0x7FFF;
		return checked(segmentBase + wordAddress * 2);
	}

	/// <summary>
	/// Allocates a new tuple from the top of the tuple stack (high end of the high segment).
	/// </summary>
	public ushort AllocateTuple(ushort wordCount)
	{
		return AllocateTupleAggregate(wordCount, []);
	}

	/// <summary>
	/// Allocates a new vector aggregate of the requested word count from the low or high arena.
	/// Reuses a previously freed block of matching size if one is available.
	/// </summary>
	public ushort AllocateVector(ushort wordCount)
	{
		return AllocateVectorAggregate(wordCount, []);
	}

	/// <summary>
	/// Allocates a new tuple and initializes it with the provided values.
	/// </summary>
	public ushort AllocateTuple(ushort wordCount, IReadOnlyList<ushort> initialValues)
	{
		return AllocateTupleAggregate(wordCount, initialValues);
	}

	/// <summary>
	/// Allocates a new vector and initializes it with the provided values.
	/// </summary>
	public ushort AllocateVector(ushort wordCount, IReadOnlyList<ushort> initialValues)
	{
		return AllocateVectorAggregate(wordCount, initialValues);
	}

	/// <summary>
	/// Returns an aggregate to the free list. Only one freed block per word count is cached,
	/// up to 64 words, matching the original interpreter's behavior.
	/// </summary>
	public void ReleaseAggregate(ushort handle, ushort size)
	{
		if (handle == FalseSentinel || size == 0)
		{
			return;
		}

		int byteOffset = ResolveVmBaseByteOffset(handle);
		int byteSize = (int)size * 2;

		managedAggregateWordCounts.Remove(handle);

		// Match MME/LinchpinST: only cache one freed block per word count,
		// up to 64 words. If a slot is already occupied, discard the block.
		if (size <= 64 && !freeVectorBlocks.ContainsKey(byteSize))
		{
			freeVectorBlocks[byteSize] = byteOffset;
		}

	}

	private ushort AllocateVectorAggregate(ushort wordCount, IReadOnlyList<ushort> initialValues)
	{
		int requiredBytes = Math.Max(1, (int)wordCount) * 2;
		int allocationByteOffset;
		bool highSegment;

		// Try to reuse a previously freed block of the same size.
		if (freeVectorBlocks.TryGetValue(requiredBytes, out int freeOffset) && freeOffset != 0)
		{
			allocationByteOffset = freeOffset;
			freeVectorBlocks.Remove(requiredBytes);
			highSegment = allocationByteOffset >= LowSegmentByteLength;
		}
		else if (nextLowArenaByteOffset + requiredBytes <= LowSegmentByteLength)
		{
			allocationByteOffset = nextLowArenaByteOffset;
			nextLowArenaByteOffset += requiredBytes;
			highSegment = false;
		}
		else if (nextHighArenaByteOffset + requiredBytes <= tupleStackFloorByteOffset)
		{
			allocationByteOffset = nextHighArenaByteOffset;
			nextHighArenaByteOffset += requiredBytes;
			highSegment = true;
		}
		else
		{
			throw new LinchpinException($"Managed vector allocation of {wordCount} words exhausted VM arena space.");
		}

		ushort handle = EncodeVmHandle(allocationByteOffset, highSegment);
		managedAggregateWordCounts[handle] = wordCount;
		Array.Clear(RamBytes, allocationByteOffset, requiredBytes);
		for (int index = 0; index < initialValues.Count && index < wordCount; index++)
		{
			WriteAggregateWord(handle, index, initialValues[index]);
		}

		return handle;
	}

	private ushort AllocateTupleAggregate(ushort wordCount, IReadOnlyList<ushort> initialValues)
	{
		int requiredBytes = Math.Max(1, (int)wordCount) * 2;
		int newTupleStackByteOffset = tupleStackByteOffset - requiredBytes;
		if (newTupleStackByteOffset < tupleStackFloorByteOffset)
		{
			throw new LinchpinException($"Tuple allocation of {wordCount} words exhausted tuple stack space.");
		}

		tupleStackByteOffset = newTupleStackByteOffset;
		ushort handle = EncodeVmHandle(tupleStackByteOffset, highSegment: true);
		managedAggregateWordCounts[handle] = wordCount;
		Array.Clear(RamBytes, tupleStackByteOffset, requiredBytes);
		for (int index = 0; index < initialValues.Count && index < wordCount; index++)
		{
			WriteAggregateWord(handle, index, initialValues[index]);
		}

		return handle;
	}

	/// <summary>
	/// Releases words from the top of the tuple stack, reclaiming space for future allocations.
	/// </summary>
	public void ReleaseTupleWords(ushort wordCount)
	{
		int releasedBytes = wordCount * 2;
		tupleStackByteOffset = Math.Min(LowSegmentByteLength + HighSegmentByteLength, tupleStackByteOffset + releasedBytes);
	}

	private static ushort EncodeVmHandle(int byteOffset, bool highSegment)
	{
		int wordAddress = (byteOffset & 0xFFFF) / 2;
		return (ushort)((highSegment ? 0x8000 : 0) | wordAddress);
	}

	/// <summary>
	/// Extracts a bit field from a word within an aggregate, where the aggregate handle
	/// is stored in a local variable identified by the control word.
	/// </summary>
	public ushort ExtractBitFieldFromLocal(ushort controlWord)
	{
		BitFieldReadSpec spec = DecodeBitFieldReadSpec(controlWord, usesLocalHandle: true);
		ushort handle = LoadLocal(spec.LocalIndex!.Value);
		ushort value = ExtractBitFieldCore(handle, spec);
		return value;
	}

	/// <summary>
	/// Extracts a bit field from a word within an aggregate at the given handle.
	/// The control word encodes the shift, width, and word index.
	/// </summary>
	public ushort ExtractBitField(ushort handle, ushort controlWord)
	{
		BitFieldReadSpec spec = DecodeBitFieldReadSpec(controlWord, usesLocalHandle: false);
		ushort value = ExtractBitFieldCore(handle, spec);
		return value;
	}

	private ushort ExtractBitFieldCore(ushort handle, BitFieldReadSpec spec)
	{
		ushort word = ReadAggregateWord(handle, spec.WordIndex);
		ushort value = (ushort)((word >> spec.Shift) & spec.ValueMask);
		return value;
	}

	public void ReplaceBitFieldInLocal(ushort controlWord, ushort value)
	{
		BitFieldWriteSpec spec = DecodeBitFieldWriteSpec(controlWord, usesLocalHandle: true);
		ReplaceBitFieldCore(LoadLocal(spec.LocalIndex!.Value), spec, value);
	}

	public void ReplaceBitField(ushort handle, ushort controlWord, ushort value)
	{
		ReplaceBitFieldCore(handle, DecodeBitFieldWriteSpec(controlWord, usesLocalHandle: false), value);
	}

	private void ReplaceBitFieldCore(ushort handle, BitFieldWriteSpec spec, ushort value)
	{
		ushort word = ReadAggregateWord(handle, spec.WordIndex);
		ushort cleared = (ushort)(word & ~spec.WordMask);
		ushort updated = (ushort)(cleared | (((ushort)(value & spec.ValueMask)) << spec.Shift));
		WriteAggregateWord(handle, spec.WordIndex, updated);
	}

	public void ReplaceSingleBitInLocal(ushort controlWord)
	{
		SingleBitSpec spec = DecodeSingleBitSpec(controlWord, usesLocalHandle: true);
		ReplaceSingleBitCore(LoadLocal(spec.LocalIndex!.Value), spec);
	}

	public void ReplaceSingleBit(ushort handle, ushort controlWord)
	{
		ReplaceSingleBitCore(handle, DecodeSingleBitSpec(controlWord, usesLocalHandle: false));
	}

	private void ReplaceSingleBitCore(ushort handle, SingleBitSpec spec)
	{
		ushort mask = (ushort)(1 << spec.BitNumber);
		ushort word = ReadAggregateWord(handle, spec.WordIndex);
		ushort updated = spec.BitValue ? (ushort)(word | mask) : (ushort)(word & ~mask);
		WriteAggregateWord(handle, spec.WordIndex, updated);
	}

	private static BitFieldReadSpec DecodeBitFieldReadSpec(ushort controlWord, bool usesLocalHandle)
	{
		int shift = (controlWord >> 12) & 0x0F;
		int width = (controlWord >> 8) & 0x0F;
		// MME uses a 16-entry width mask table where width 0 maps to 0 and
		// widths that extend past the end of the word are truncated naturally
		// by the 16-bit shifts and masks in the handler.
		ushort valueMask = width == 0 ? (ushort)0 : (ushort)((1 << width) - 1);

		int? localIndex = usesLocalHandle ? (controlWord >> 4) & 0x0F : null;
		int wordIndex = usesLocalHandle ? controlWord & 0x0F : controlWord & 0xFF;
		return new BitFieldReadSpec(shift, valueMask, wordIndex, localIndex);
	}

	private static BitFieldWriteSpec DecodeBitFieldWriteSpec(ushort controlWord, bool usesLocalHandle)
	{
		BitFieldReadSpec readSpec = DecodeBitFieldReadSpec(controlWord, usesLocalHandle);
		ushort wordMask = (ushort)(readSpec.ValueMask << readSpec.Shift);
		return new BitFieldWriteSpec(readSpec.Shift, readSpec.ValueMask, wordMask, readSpec.WordIndex, readSpec.LocalIndex);
	}

	private static SingleBitSpec DecodeSingleBitSpec(ushort controlWord, bool usesLocalHandle)
	{
		int bitNumber = (controlWord >> 12) & 0x0F;
		bool bitValue = ((controlWord >> 8) & 0x01) != 0;
		int? localIndex = usesLocalHandle ? (controlWord >> 4) & 0x0F : null;
		int wordIndex = usesLocalHandle ? controlWord & 0x0F : controlWord & 0xFF;
		return new SingleBitSpec(bitNumber, bitValue, wordIndex, localIndex);
	}
}
