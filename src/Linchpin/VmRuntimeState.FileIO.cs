/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Text;

namespace Linchpin;

internal sealed partial class VmRuntimeState
{
	/// <summary>
	/// Opens a synthetic file channel. Pops a discard value and the file name handle from the stack,
	/// resolves the file from the synthetic file system or host disk, and returns a channel ID.
	/// Returns <c>FalseSentinel</c> if the file cannot be found or created.
	/// </summary>
	/// <param name="mode">The open mode byte from the OPEN opcode.</param>
	public ushort OpenSyntheticChannel(int mode)
	{
		Pop();
		ushort fileNameHandle = Pop();
		string fileName = ReadVmString(fileNameHandle);
		string sanitizedName = SanitizeHostFileName(fileName);

		if (string.IsNullOrWhiteSpace(sanitizedName))
		{
			return FalseSentinel;
		}

		if (IsCreateFirstChannelMode(mode))
		{
			string? outputPath = null;
			if (executionOptions.HostWriteFiles)
			{
				outputPath = ResolveSyntheticOutputPath(sanitizedName);
				Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
				File.WriteAllBytes(outputPath, Array.Empty<byte>());
			}

			deletedSyntheticFiles.Remove(sanitizedName);
			byte[] contents = Array.Empty<byte>();
			syntheticFiles[sanitizedName] = contents;
			ushort syntheticChannelId = CreateChannelId();
			openChannels[syntheticChannelId] = new VmChannel(fileName, outputPath, contents, mode, 0);
			return syntheticChannelId;
		}

		if (syntheticFiles.TryGetValue(sanitizedName, out byte[]? existingSyntheticContents))
		{
			ushort syntheticChannelId = CreateChannelId();
			string? resolvedPath = TryResolveSyntheticFilePath(sanitizedName);
			openChannels[syntheticChannelId] = new VmChannel(fileName, ResolveChannelPath(mode, resolvedPath), existingSyntheticContents, mode, 0);
			return syntheticChannelId;
		}

		string? fallbackPath = TryResolveSyntheticFilePath(sanitizedName);
		if (!string.IsNullOrWhiteSpace(fallbackPath))
		{
			byte[] contents = File.ReadAllBytes(fallbackPath);
			syntheticFiles[sanitizedName] = contents;
			ushort syntheticChannelId = CreateChannelId();
			openChannels[syntheticChannelId] = new VmChannel(fileName, ResolveChannelPath(mode, fallbackPath), contents, mode, 0);
			return syntheticChannelId;
		}

		if (deletedSyntheticFiles.Contains(sanitizedName))
		{
			return 0x8001;
		}

		return 0x8001;
	}

	/// <summary>
	/// Closes a channel, flushing its contents back to the synthetic file system and to disk
	/// if the channel has a host path.
	/// </summary>
	public ushort CloseSyntheticChannel(ushort channelId)
	{
		if (openChannels.TryGetValue(channelId, out VmChannel? channel))
		{
			syntheticFiles[SanitizeHostFileName(channel.Name)] = channel.Contents;
			if (channel.Path is not null)
			{
				Directory.CreateDirectory(Path.GetDirectoryName(channel.Path)!);
				File.WriteAllBytes(channel.Path, channel.Contents);
			}
		}

		openChannels.Remove(channelId);
		if (activeChannelId == channelId)
		{
			activeChannelId = FalseSentinel;
		}
		return 0;
	}

	/// <summary>
	/// Returns the size of the channel's contents in 256-byte blocks.
	/// </summary>
	public ushort GetChannelSizeBlocks(ushort channelId)
	{
		if (!TryResolveChannel(channelId, out ushort resolvedChannelId, out VmChannel channel))
		{
			return 0;
		}

		return (ushort)(channel.Contents.Length / 0x100);
	}

	/// <summary>
	/// Reads sequential bytes from a channel into a VM vector, advancing the channel's position.
	/// </summary>
	public ushort ReadChannel(ushort vectorHandle, ushort channelId, ushort wordCount)
	{
		if (!TryResolveChannel(channelId, out ushort resolvedChannelId, out VmChannel channel))
		{
			FillAggregateBytes(vectorHandle, checked((ushort)(wordCount * 2)), 0);
			Trace($"READ channel={channelId} words={wordCount} -> missing");
			return 0xFFFF;
		}

		int transferCount = checked(wordCount * 2);
		int available = Math.Max(0, channel.Contents.Length - channel.Position);
		int copied = Math.Min(transferCount, available);

		for (int index = 0; index < copied; index++)
		{
			WriteAggregateByte(vectorHandle, index, channel.Contents[channel.Position + index]);
		}

		for (int index = copied; index < transferCount; index++)
		{
			WriteAggregateByte(vectorHandle, index, 0);
		}

		openChannels[resolvedChannelId] = channel with { Position = channel.Position + copied };
		Trace($"READ channel={resolvedChannelId} words={wordCount} copied={copied} next={channel.Position + copied}");
		return copied == transferCount ? (ushort)0 : (ushort)0xFFFF;
	}

	/// <summary>
	/// Writes sequential bytes from a VM vector into a channel, advancing the channel's position.
	/// </summary>
	public ushort WriteChannel(ushort vectorHandle, ushort channelId, ushort wordCount)
	{
		if (!TryResolveChannel(channelId, out ushort resolvedChannelId, out VmChannel channel))
		{
			Trace($"WRITE channel={channelId} words={wordCount} -> missing");
			return 0xFFFF;
		}

		int transferCount = wordCount;
		int requiredLength = channel.Position + transferCount;
		byte[] contents = channel.Contents;
		if (requiredLength > contents.Length)
		{
			Array.Resize(ref contents, requiredLength);
		}

		for (int index = 0; index < transferCount; index++)
		{
			contents[channel.Position + index] = (byte)ReadAggregateByte(vectorHandle, index * 2);
		}

		syntheticFiles[SanitizeHostFileName(channel.Name)] = contents;
		if (channel.Path is not null)
		{
			Directory.CreateDirectory(Path.GetDirectoryName(channel.Path)!);
			File.WriteAllBytes(channel.Path, contents);
		}

		openChannels[resolvedChannelId] = channel with { Contents = contents, Position = channel.Position + transferCount };
		Trace($"WRITE channel={resolvedChannelId} words={wordCount} bytes={transferCount} next={channel.Position + transferCount}");
		return 0;
	}

	/// <summary>
	/// Reads a fixed-size record from a channel into a VM vector at a record-aligned offset.
	/// </summary>
	public ushort ReadRecord(ushort vectorHandle, ushort channelId, ushort record, ushort wordCount)
	{
		if (!TryResolveChannel(channelId, out ushort resolvedChannelId, out VmChannel channel))
		{
			int missingTransferBytes = GetRecordTransferByteCount(wordCount);
			FillAggregateBytes(vectorHandle, checked((ushort)missingTransferBytes), 0);
			Trace($"READREC channel={channelId} record={record} words={wordCount} -> missing");
			return 0;
		}

		int recordSizeBytes = GetRecordSizeInBytes();
		int sourceOffset = checked(record * recordSizeBytes);
		int byteCount = GetRecordTransferByteCount(wordCount);
		int available = Math.Max(0, channel.Contents.Length - sourceOffset);
		int copied = Math.Min(byteCount, available);
		int wordTransferCount = byteCount / 2;
		for (int wordIndex = 0; wordIndex < wordTransferCount; wordIndex++)
		{
			int bytePos = wordIndex * 2;
			byte low = bytePos < copied ? channel.Contents[sourceOffset + bytePos] : (byte)0;
			byte high = (bytePos + 1) < copied ? channel.Contents[sourceOffset + bytePos + 1] : (byte)0;
			WriteAggregateWord(vectorHandle, wordIndex, (ushort)(low | (high << 8)));
		}

		Trace($"READREC channel={resolvedChannelId} record={record} records={wordCount} bytes={copied}/{byteCount} source=0x{sourceOffset:X}");
		return checked((ushort)copied);
	}

	/// <summary>
	/// Writes a fixed-size record from a VM vector into a channel at a record-aligned offset.
	/// </summary>
	public ushort WriteRecord(ushort vectorHandle, ushort channelId, ushort record, ushort wordCount)
	{
		if (!TryResolveChannel(channelId, out ushort resolvedChannelId, out VmChannel channel))
		{
			Trace($"WRITEREC channel={channelId} record={record} words={wordCount} -> missing");
			return 0;
		}

		int recordSizeBytes = GetRecordSizeInBytes();
		int targetOffset = checked(record * recordSizeBytes);
		int byteCount = GetRecordTransferByteCount(wordCount);
		int requiredLength = targetOffset + byteCount;
		byte[] contents = channel.Contents;
		if (requiredLength > contents.Length)
		{
			Array.Resize(ref contents, requiredLength);
		}

		int wordTransferCount = byteCount / 2;
		for (int wordIndex = 0; wordIndex < wordTransferCount; wordIndex++)
		{
			ushort word = ReadAggregateWord(vectorHandle, wordIndex);
			int bytePos = targetOffset + wordIndex * 2;
			contents[bytePos] = (byte)(word & 0xFF);
			contents[bytePos + 1] = (byte)(word >> 8);
		}

		syntheticFiles[SanitizeHostFileName(channel.Name)] = contents;
		if (channel.Path is not null)
		{
			Directory.CreateDirectory(Path.GetDirectoryName(channel.Path)!);
			File.WriteAllBytes(channel.Path, contents);
		}

		openChannels[resolvedChannelId] = channel with { Contents = contents };
		Trace($"WRITEREC channel={resolvedChannelId} record={record} records={wordCount} bytes={byteCount} target=0x{targetOffset:X}");
		return checked((ushort)byteCount);
	}

	private bool TryResolveChannel(ushort channelId, out ushort resolvedChannelId, out VmChannel channel)
	{
		if (channelId == FalseSentinel)
		{
			if (activeChannelId != FalseSentinel && openChannels.TryGetValue(activeChannelId, out VmChannel? activeChannel))
			{
				channel = activeChannel!;
				resolvedChannelId = activeChannelId;
				return true;
			}

			resolvedChannelId = channelId;
			channel = null!;
			return false;
		}

		if (openChannels.TryGetValue(channelId, out VmChannel? explicitChannel))
		{
			channel = explicitChannel!;
			activeChannelId = channelId;
			resolvedChannelId = channelId;
			return true;
		}

		resolvedChannelId = channelId;
		channel = null!;
		return false;
	}

	private int GetRecordSizeInBytes()
	{
		int recordSize = LoadModuleGlobal(0xCC);
		if (recordSize <= 0)
		{
			return 0x100;
		}

		return recordSize < 0x80 ? checked(recordSize * 2) : recordSize;
	}

	private int GetRecordTransferByteCount(ushort recordCount)
	{
		return checked(recordCount * GetRecordSizeInBytes());
	}

	private string ReadVmString(ushort handle)
	{
		ushort byteLength = ReadAggregateWord(handle, 0);
		ReadOnlySpan<byte> bytes = ReadAggregatePayloadBytes(handle, byteLength, 0);
		string raw = Encoding.ASCII.GetString(bytes);
		int nulIndex = raw.IndexOf('\0');
		if (nulIndex >= 0)
		{
			raw = raw[..nulIndex];
		}

		return raw.Trim();
	}

	/// <summary>
	/// Searches for a byte value in a VM string's payload bytes, returning its 1-based index
	/// or <c>FalseSentinel</c> if not found.
	/// </summary>
	public ushort FindCharacterInVmString(ushort handle, byte character)
	{
		ushort byteLength = ReadAggregateWord(handle, 0);
		ReadOnlySpan<byte> bytes = ReadAggregatePayloadBytes(handle, byteLength, 0);

		for (int index = 0; index < bytes.Length; index++)
		{
			if (bytes[index] == 0)
			{
				break;
			}

			if (bytes[index] == character)
			{
				return checked((ushort)(index + 1));
			}
		}

		return 0x8001;
	}

	/// <summary>
	/// Performs a case-insensitive comparison of two VM string regions, using the same
	/// convention as the interpreter: +1 if first &lt; second, 0 if equal, -1 (0xFFFF) if first &gt; second.
	/// </summary>
	public ushort CompareVmStringsIgnoreCase(ushort firstHandle, ushort firstOffset, ushort firstLength, ushort secondHandle, ushort secondLength)
	{
		ReadOnlySpan<byte> firstBytes = ReadAggregatePayloadBytes(firstHandle, firstLength, firstOffset);
		ReadOnlySpan<byte> secondBytes = ReadAggregatePayloadBytes(secondHandle, secondLength, 0);

		int firstIndex = 0;
		int secondIndex = 0;

		while (true)
		{
			bool firstHasValue = TryReadIgnoreCaseCompareByte(firstBytes, ref firstIndex, out byte firstValue);
			bool secondHasValue = TryReadIgnoreCaseCompareByte(secondBytes, ref secondIndex, out byte secondValue);

			if (!firstHasValue || !secondHasValue)
			{
				if (firstHasValue == secondHasValue)
				{
					return 0;
				}

				// MME convention: first longer → -1, second longer → +1
				return firstHasValue ? unchecked((ushort)-1) : (ushort)1;
			}

			if (firstValue == secondValue)
			{
				continue;
			}

			return firstValue < secondValue ? (ushort)1 : unchecked((ushort)-1);
		}
	}

	/// <summary>
	/// Compares two structured sort key aggregates using lexicographic comparison.
    /// This is NOT a simple NUL-terminated byte comparison.
	/// </summary>
	/// <remarks>
	/// The algorithm reads a "field count" from payload byte -1 (= raw byte 1 = high
	/// byte of the first word) of each handle. Then it enters a structured comparison loop:
	/// read a sub-key length from each handle, compare that many data bytes, and repeat until
	/// all sub-keys are exhausted.
	/// </remarks>
	public ushort CompareSortKeys(ushort firstHandle, ushort secondHandle)
	{
		// Payload byte -1 = raw byte 1 = high byte of word 0 = "remaining byte count"
		int firstRemaining = (byte)ReadAggregateByte(firstHandle, -1);
		int secondRemaining = (byte)ReadAggregateByte(secondHandle, -1);

		int payloadIndex = 4; // Data starts at payload byte 4 (raw byte 6 of handle)

		while (true)
		{
			// Phase 1: Check if either remaining count is zero
			if (firstRemaining == 0)
			{
				if (secondRemaining == 0)
				{
					// Both exhausted — check bit 6 of payload byte -2 for structured flag.
					// If set, return 0 (equal). If not, return -1 or jump to tie-break.
					byte flagByte = (byte)ReadAggregateByte(firstHandle, -2);
					if ((flagByte & 0x40) == 0)
					{
						// Not a structured sort key — return 0 (treated as equal)
						return 0;
					}

					// Structured sort key — compare combined header values for tie-breaking.
					// Read payload byte 3 (6-bit field) and payload byte 2 from each handle.
					int firstCombined = ((byte)ReadAggregateByte(firstHandle, 3) & 0x3F) << 8
						| (byte)ReadAggregateByte(firstHandle, 2);
					int secondCombined = ((byte)ReadAggregateByte(secondHandle, 3) & 0x3F) << 8
						| (byte)ReadAggregateByte(secondHandle, 2);

					if (firstCombined < secondCombined)
						return unchecked((ushort)-1);
					if (firstCombined > secondCombined)
						return 1;

					// Still equal — compare payload byte -2 (6-bit)
					int firstFlag = (byte)ReadAggregateByte(firstHandle, -2) & 0x3F;
					int secondFlag = (byte)ReadAggregateByte(secondHandle, -2) & 0x3F;

					if (firstFlag < secondFlag)
						return unchecked((ushort)-1);

					// MME returns +1 for >= (no true "equal" path for this case)
					return 1;
				}

				// First exhausted, second still has data → first < second
				return unchecked((ushort)-1);
			}

			if (secondRemaining == 0)
			{
				// First has data, second exhausted → first > second
				return 1;
			}

			// Phase 2: Read sub-key length from each handle at the current payload index
			int firstFieldLen = (byte)ReadAggregateByte(firstHandle, payloadIndex);
			int secondFieldLen = (byte)ReadAggregateByte(secondHandle, payloadIndex);

			payloadIndex++;

			// Update remaining counts: remaining -= fieldLen + 1
			firstRemaining -= firstFieldLen + 1;
			secondRemaining -= secondFieldLen + 1;

			// Phase 3: Compare min(len1, len2) data bytes
			int compareCount = Math.Min(firstFieldLen, secondFieldLen);

			for (int i = 0; i < compareCount; i++)
			{
				byte firstByte = (byte)ReadAggregateByte(firstHandle, payloadIndex);
				byte secondByte = (byte)ReadAggregateByte(secondHandle, payloadIndex);
				payloadIndex++;

				if (firstByte > secondByte)
					return 1;
				if (firstByte < secondByte)
					return unchecked((ushort)-1);
			}

			// All compared bytes were equal — check if lengths differ
			if (firstFieldLen > secondFieldLen)
			{
				// First has more data bytes → first > second
				return 1;
			}

			if (firstFieldLen < secondFieldLen)
			{
				// Second has more → first < second
				return unchecked((ushort)-1);
			}

			// Field lengths equal, all bytes equal → skip the equal-length bytes
			// and continue to the next sub-key (loop back)
			// payloadIndex is already advanced past the compared bytes
		}
	}

	public ushort CompareAggregateBytes(ushort firstHandle, ushort firstOffset, ushort secondHandle, ushort secondOffset, ushort byteCount)
	{
		for (int index = 0; index < byteCount; index++)
		{
			byte firstValue = (byte)ReadAggregateByte(firstHandle, firstOffset + index);
			byte secondValue = (byte)ReadAggregateByte(secondHandle, secondOffset + index);
			if (firstValue == secondValue)
			{
				continue;
			}

			// first < second → positive (+1); first > second → negative (-1)
			return firstValue < secondValue ? (ushort)1 : unchecked((ushort)-1);
		}

		return 0;
	}

	public ushort CompareAggregateRawBytesToPayload(ushort firstHandle, ushort firstOffset, ushort secondHandle, ushort secondOffset, ushort byteCount)
	{
		for (int index = 0; index < byteCount; index++)
		{
			byte firstValue = (byte)ReadAggregateRawByte(firstHandle, firstOffset + index);
			byte secondValue = (byte)ReadAggregateByte(secondHandle, secondOffset + index);
			if (firstValue == secondValue)
			{
				continue;
			}

			// first < second → positive (+1); first > second → negative (-1)
			return firstValue < secondValue ? (ushort)1 : unchecked((ushort)-1);
		}

		return 0;
	}

	private static bool TryReadIgnoreCaseCompareByte(ReadOnlySpan<byte> bytes, ref int index, out byte value)
	{
		if (index < bytes.Length)
		{
			value = ToUpperAscii(bytes[index++]);
			return true;
		}

		value = 0;
		return false;
	}

	private static byte ToUpperAscii(byte value)
	{
		return value is >= (byte)'a' and <= (byte)'z'
			? (byte)(value - 0x20)
			: value;
	}

	/// <summary>
	/// Removes a synthetic file from the in-memory file system and closes any open channels for it.
	/// Returns 0 on success, <c>FalseSentinel</c> if the file was not found.
	/// </summary>
	public ushort UnlinkFile(ushort fileNameHandle)
	{
		string fileName = ReadVmString(fileNameHandle);
		string sanitizedName = SanitizeHostFileName(fileName);
		bool removed = false;

		if (syntheticFiles.Remove(sanitizedName))
		{
			deletedSyntheticFiles.Add(sanitizedName);
			removed = true;
		}

		ushort[] openSyntheticChannels = openChannels
			.Where(pair => pair.Value.Path is null && string.Equals(SanitizeHostFileName(pair.Value.Name), sanitizedName, StringComparison.OrdinalIgnoreCase))
			.Select(pair => pair.Key)
			.ToArray();

		foreach (ushort channelId in openSyntheticChannels)
		{
			openChannels.Remove(channelId);
			removed = true;
		}

		Trace($"UNLINK name='{fileName}' syntheticRemoved={removed}");
		return removed ? (ushort)0 : FalseSentinel;
	}

	/// <summary>
	/// Unpacks 5-bit-packed characters from a source aggregate into a destination aggregate.
	/// Each source word contains three 5-bit characters; bit 15 marks the final word.
	/// </summary>
	public ushort ExecuteUnpack()
	{
		if (evaluationStack.Count < 4)
		{
			Trace($"UNPACK -> false (stack depth {evaluationStack.Count})");
			return FalseSentinel;
		}

		ushort destinationOffset = Pop();
		ushort wordCount = Pop();
		ushort destinationHandle = Pop();
		ushort sourceHandle = Pop();

		for (int wordIndex = 0; wordIndex < wordCount; wordIndex++)
		{
			ushort packedWord = ReadAggregateWord(sourceHandle, wordIndex);
			byte first = (byte)((packedWord >> 10) & 0x1F);
			byte second = (byte)((packedWord >> 5) & 0x1F);
			byte third = (byte)(packedWord & 0x1F);

			WriteAggregateByte(destinationHandle, destinationOffset++, first);
			WriteAggregateByte(destinationHandle, destinationOffset++, second);

			if ((packedWord & 0x8000) != 0)
			{
				WriteAggregateByte(destinationHandle, destinationOffset++, (byte)(third | 0x80));
				Trace($"UNPACK src=0x{sourceHandle:X4} dst=0x{destinationHandle:X4} words={wordCount} offset=0x{destinationOffset:X4} -> false");
				return FalseSentinel;
			}

			WriteAggregateByte(destinationHandle, destinationOffset++, third);
		}

		Trace($"UNPACK src=0x{sourceHandle:X4} dst=0x{destinationHandle:X4} words={wordCount} -> 0x{destinationOffset:X4}");
		return destinationOffset;
	}

	private void LoadInitialSyntheticFiles()
	{
		string? configuredDataDirectory = GetConfiguredDataDirectory();
		if (!string.IsNullOrWhiteSpace(configuredDataDirectory))
		{
			if (Directory.Exists(configuredDataDirectory))
			{
				LoadSyntheticFilesFromDirectory(configuredDataDirectory);
			}

			LoadInstalledSampleAliases();
			return;
		}

		// Allow an installed database directory to be provided explicitly via environment.
		string? installedDir = Environment.GetEnvironmentVariable("LINCHPIN_DATA_DIR");
		if (!string.IsNullOrEmpty(installedDir) && Directory.Exists(installedDir))
		{
			LoadSyntheticFilesFromDirectory(installedDir);
			LoadInstalledSampleAliases();
			return;
		}

		string dataDirectory = Path.Combine(repositoryRoot, "Sample");
		if (Directory.Exists(dataDirectory))
		{
			LoadSyntheticFilesFromDirectory(dataDirectory);
		}

		LoadInstalledSampleAliases();
	}

	private void LoadSyntheticFilesFromDirectory(string dataDirectory)
	{
		foreach (string path in Directory.EnumerateFiles(dataDirectory))
		{
			string sanitizedName = SanitizeHostFileName(Path.GetFileName(path));
			if (string.IsNullOrWhiteSpace(sanitizedName))
			{
				continue;
			}

			syntheticFiles[sanitizedName] = File.ReadAllBytes(path);
		}
	}

	private void LoadInstalledSampleAliases()
	{
		AddExtensionAliases(".NEW", ".DBF");
		AddExtensionAliases(".CNR", ".DBF");

		// Cornerstone installers materialize SY000000 from either the color or B/W template.
		if (!AddSyntheticAlias("SYCO.NEW", "SY000000.DBF"))
		{
			AddSyntheticAlias("SYBW.NEW", "SY000000.DBF");
		}

		if (!AddSyntheticAlias("SYCO.NEW", "SY000000.SLS"))
		{
			AddSyntheticAlias("SYBW.NEW", "SY000000.SLS");
		}

	}

	private string? TryResolveSyntheticFilePath(string sanitizedName)
	{
		if (string.IsNullOrWhiteSpace(sanitizedName))
		{
			return null;
		}

		foreach (string candidateFileName in EnumerateSyntheticFileNameCandidates(sanitizedName))
		{
			foreach (string directory in EnumerateSyntheticDataDirectories())
			{
				string candidatePath = Path.Combine(directory, candidateFileName);
				if (File.Exists(candidatePath))
				{
					return candidatePath;
				}
			}
		}

		return null;
	}

	private IEnumerable<string> EnumerateSyntheticDataDirectories()
	{
		HashSet<string> seen = new(StringComparer.OrdinalIgnoreCase);
		string? dataDirectory = GetPrimaryDataDirectory();
		foreach (string directory in EnumerateDistinctSearchDirectories(dataDirectory, seen))
		{
			yield return directory;
		}
	}

	private IEnumerable<string> EnumerateDistinctSearchDirectories(string? dataDirectory, HashSet<string> seen)
	{
		if (!string.IsNullOrWhiteSpace(dataDirectory) && Directory.Exists(dataDirectory) && seen.Add(dataDirectory))
		{
			yield return dataDirectory;
		}

		string? dataDirectoryParent = GetParentDirectory(dataDirectory);
		if (!string.IsNullOrWhiteSpace(dataDirectoryParent) && Directory.Exists(dataDirectoryParent) && seen.Add(dataDirectoryParent))
		{
			yield return dataDirectoryParent;
		}

		string? mmeDirectory = Path.GetDirectoryName(image.MmePath);
		if (!string.IsNullOrWhiteSpace(mmeDirectory) && Directory.Exists(mmeDirectory) && seen.Add(mmeDirectory))
		{
			yield return mmeDirectory;
		}
	}

	private IEnumerable<string> EnumerateSyntheticFileNameCandidates(string sanitizedName)
	{
		HashSet<string> seen = new(StringComparer.OrdinalIgnoreCase)
		{
			sanitizedName,
		};

		yield return sanitizedName;

		if (sanitizedName.Equals("SY000000.DBF", StringComparison.OrdinalIgnoreCase))
		{
			if (seen.Add("SYCO.NEW"))
			{
				yield return "SYCO.NEW";
			}

			if (seen.Add("SYBW.NEW"))
			{
				yield return "SYBW.NEW";
			}
		}

		if (sanitizedName.Equals("SY000000.SLS", StringComparison.OrdinalIgnoreCase))
		{
			if (seen.Add("SYCO.NEW"))
			{
				yield return "SYCO.NEW";
			}

			if (seen.Add("SYBW.NEW"))
			{
				yield return "SYBW.NEW";
			}
		}

		foreach (string alias in EnumerateExtensionAliases(sanitizedName))
		{
			if (seen.Add(alias))
			{
				yield return alias;
			}
		}
	}

	private static IEnumerable<string> EnumerateExtensionAliases(string sanitizedName)
	{
		if (sanitizedName.EndsWith(".DBF", StringComparison.OrdinalIgnoreCase))
		{
			yield return sanitizedName[..^4] + ".NEW";
		}

		if (sanitizedName.EndsWith(".CNR", StringComparison.OrdinalIgnoreCase))
		{
			yield return sanitizedName[..^4] + ".NEW";
			yield return sanitizedName[..^4] + ".DBF";
		}

		if (sanitizedName.EndsWith(".SLS", StringComparison.OrdinalIgnoreCase))
		{
			yield return sanitizedName[..^4] + ".NEW";
		}
	}

	private string? GetConfiguredDataDirectory()
	{
		if (!string.IsNullOrWhiteSpace(executionOptions.DataDirectoryPath))
		{
			return executionOptions.DataDirectoryPath;
		}

		return null;
	}

	private string? GetPrimaryDataDirectory()
	{
		string? configuredDataDirectory = GetConfiguredDataDirectory();
		if (!string.IsNullOrWhiteSpace(configuredDataDirectory))
		{
			return configuredDataDirectory;
		}

		string? installedDir = Environment.GetEnvironmentVariable("LINCHPIN_DATA_DIR");
		if (!string.IsNullOrWhiteSpace(installedDir))
		{
			return installedDir;
		}

		return Path.Combine(repositoryRoot, "Sample");
	}

	private static string? GetParentDirectory(string? path)
	{
		if (string.IsNullOrWhiteSpace(path))
		{
			return null;
		}

		return Directory.GetParent(path)?.FullName;
	}

	private bool AddSyntheticAlias(string sourceFileName, string aliasFileName)
	{
		string sanitizedSource = SanitizeHostFileName(sourceFileName);
		string sanitizedAlias = SanitizeHostFileName(aliasFileName);
		if (string.IsNullOrWhiteSpace(sanitizedSource) || string.IsNullOrWhiteSpace(sanitizedAlias))
		{
			return false;
		}

		if (!syntheticFiles.TryGetValue(sanitizedSource, out byte[]? contents))
		{
			return false;
		}

		if (syntheticFiles.ContainsKey(sanitizedAlias))
		{
			return true;
		}

		byte[] copy = new byte[contents.Length];
		Buffer.BlockCopy(contents, 0, copy, 0, contents.Length);
		syntheticFiles[sanitizedAlias] = copy;
		return true;
	}

	private void AddExtensionAliases(string sourceExtension, string aliasExtension)
	{
		foreach (string sourceName in syntheticFiles.Keys.ToArray())
		{
			if (!sourceName.EndsWith(sourceExtension, StringComparison.OrdinalIgnoreCase))
			{
				continue;
			}

			string aliasName = sourceName[..^sourceExtension.Length] + aliasExtension;
			AddSyntheticAlias(sourceName, aliasName);
		}
	}

	private static string SanitizeHostFileName(string fileName)
	{
		char[] invalidChars = Path.GetInvalidFileNameChars();
		StringBuilder builder = new(fileName.Length);
		foreach (char character in fileName)
		{
			if (character == '\0')
			{
				break;
			}

			if (!invalidChars.Contains(character))
			{
				builder.Append(character);
			}
		}

		return builder.ToString().Trim();
	}

	private static int GetOpenAccessFamily(int mode)
	{
		return mode & 0x03;
	}

	private static bool IsWriteCapableChannelMode(int mode)
	{
		return GetOpenAccessFamily(mode) is 0x01 or 0x02 or 0x03;
	}

	private static bool IsCreateFirstChannelMode(int mode)
	{
		return GetOpenAccessFamily(mode) is 0x01 or 0x03;
	}

	private string? ResolveChannelPath(int mode, string? resolvedPath)
	{
		if (!IsWriteCapableChannelMode(mode))
		{
			return resolvedPath;
		}

		return executionOptions.HostWriteFiles ? resolvedPath : null;
	}

	private string ResolveSyntheticOutputPath(string sanitizedName)
	{
		string? configuredDataDirectory = GetConfiguredDataDirectory();
		if (!string.IsNullOrWhiteSpace(configuredDataDirectory))
		{
			return Path.Combine(configuredDataDirectory, sanitizedName);
		}

		foreach (string directory in EnumerateSyntheticDataDirectories())
		{
			if (Directory.Exists(directory))
			{
				return Path.Combine(directory, sanitizedName);
			}
		}

		string? mmeDirectory = Path.GetDirectoryName(image.MmePath);
		return Path.Combine(mmeDirectory ?? repositoryRoot, sanitizedName);
	}

	private ushort CreateChannelId()
	{
		ushort channelId = nextChannelId++;
		if (channelId == 0x8001)
		{
			channelId = nextChannelId++;
		}

		return channelId;
	}
}
