/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Linchpin;

/// <summary>
/// Loads a Cornerstone VM image from its MME (metadata) and OBJ (bytecode) files,
/// producing a <see cref="CornerstoneImage"/> that represents the complete image.
/// </summary>
internal static class CornerstoneImageLoader
{
	private const int HeaderWordCount = 48;
	private const int HeaderByteCount = HeaderWordCount * 2;

	/// <summary>
	/// Loads and validates a Cornerstone VM image from the specified MME and OBJ file paths.
	/// </summary>
	/// <param name="mmePath">Path to the .MME metadata file.</param>
	/// <param name="objPath">Path to the .OBJ bytecode file.</param>
	/// <returns>The loaded image.</returns>
	/// <exception cref="LinchpinException">Thrown if the files are missing, truncated, or structurally invalid.</exception>
	public static CornerstoneImage Load(string mmePath, string objPath)
	{
		byte[] metadataBytes = File.ReadAllBytes(mmePath);
		byte[] objectBytes = File.ReadAllBytes(objPath);

		if (metadataBytes.Length < HeaderByteCount)
		{
			throw new LinchpinException($"MME file '{mmePath}' is too small to contain a valid header.");
		}

		ushort[] headerWords = ReadWords(metadataBytes, 0, HeaderWordCount);
		int codeEndOffset = checked((headerWords[7] + 1) * 0x200);
		EntryPoint entryPoint = new(headerWords[8] >> 8, headerWords[8] & 0xFF);
		int moduleHeaderOffset = headerWords[13] * 0x100;
		int moduleHeaderLengthWords = headerWords[15];
		int moduleHeaderLengthBytes = moduleHeaderLengthWords * 2;

		if (codeEndOffset > objectBytes.Length)
		{
			throw new LinchpinException($"OBJ file '{objPath}' is shorter than the code end declared by the MME header (0x{codeEndOffset:X}).");
		}

		if (moduleHeaderOffset + moduleHeaderLengthBytes > metadataBytes.Length)
		{
			throw new LinchpinException($"MME file '{mmePath}' truncates the module header table.");
		}

		int cursor = HeaderByteCount;
		ushort moduleCount = ReadWord(metadataBytes, ref cursor, mmePath, "module count");
		List<int> moduleOffsetPages = new(moduleCount);
		for (int index = 0; index < moduleCount; index++)
		{
			moduleOffsetPages.Add(ReadWord(metadataBytes, ref cursor, mmePath, $"module offset page {index + 1}"));
		}

		ushort programGlobalCount = ReadWord(metadataBytes, ref cursor, mmePath, "program global count");
		List<ushort> moduleGlobalCounts = new(moduleCount);
		for (int index = 0; index < moduleCount; index++)
		{
			moduleGlobalCounts.Add(ReadWord(metadataBytes, ref cursor, mmePath, $"module global count {index + 1}"));
		}

		List<ushort> programGlobals = new(programGlobalCount);
		for (int index = 0; index < programGlobalCount; index++)
		{
			programGlobals.Add(ReadWord(metadataBytes, ref cursor, mmePath, $"program global initializer {index}"));
		}

		List<IReadOnlyList<ushort>> moduleGlobals = new(moduleCount);
		for (int moduleIndex = 0; moduleIndex < moduleCount; moduleIndex++)
		{
			List<ushort> values = new(moduleGlobalCounts[moduleIndex]);
			for (int valueIndex = 0; valueIndex < moduleGlobalCounts[moduleIndex]; valueIndex++)
			{
				values.Add(ReadWord(metadataBytes, ref cursor, mmePath, $"module {moduleIndex + 1} global initializer {valueIndex}"));
			}

			moduleGlobals.Add(values);
		}

		List<ModuleHeaderTableEntry> moduleHeaderTable = ParseModuleHeaderTable(metadataBytes, moduleHeaderOffset, moduleHeaderLengthBytes, moduleCount, mmePath);
		int initialRamOffset = AlignToPage(moduleHeaderOffset + moduleHeaderLengthBytes);
		int initialRamByteCount = InferInitialRamByteCount(headerWords[11], metadataBytes.Length - initialRamOffset, mmePath);
		if (initialRamOffset + initialRamByteCount > metadataBytes.Length)
		{
			throw new LinchpinException($"MME file '{mmePath}' truncates the initial RAM image.");
		}

		if (entryPoint.ModuleId < 1 || entryPoint.ModuleId > moduleCount)
		{
			throw new LinchpinException($"MME file '{mmePath}' declares an entry module outside the module table: {entryPoint.ModuleId}.");
		}

		List<ModuleImage> modules = ParseModules(objectBytes, codeEndOffset, moduleOffsetPages, moduleHeaderTable, objPath);
		if (entryPoint.ProcedureIndex >= modules[entryPoint.ModuleId - 1].Procedures.Count)
		{
			throw new LinchpinException($"MME file '{mmePath}' declares entry procedure {entryPoint.ProcedureIndex} outside module {entryPoint.ModuleId}.");
		}

		GlobalLayout globals = new(programGlobalCount, moduleGlobalCounts, programGlobals, moduleGlobals);
		byte[] initialRamBytes = metadataBytes.AsSpan(initialRamOffset, initialRamByteCount).ToArray();

		return new CornerstoneImage(
			mmePath,
			objPath,
			headerWords,
			metadataBytes,
			objectBytes,
			entryPoint,
			codeEndOffset,
			moduleHeaderOffset,
			moduleHeaderLengthWords,
			initialRamOffset,
			initialRamBytes,
			DescribeRamLengthUnits(headerWords[11], initialRamByteCount),
			globals,
			modules);
	}

	private static List<ModuleHeaderTableEntry> ParseModuleHeaderTable(byte[] metadataBytes, int moduleHeaderOffset, int moduleHeaderLengthBytes, int moduleCount, string mmePath)
	{
		int cursor = moduleHeaderOffset;
		int endOffset = moduleHeaderOffset + moduleHeaderLengthBytes;
		List<ModuleHeaderTableEntry> moduleHeaders = new(moduleCount);

		for (int moduleIndex = 0; moduleIndex < moduleCount; moduleIndex++)
		{
			ushort procedureCount = ReadWord(metadataBytes, ref cursor, mmePath, $"module {moduleIndex + 1} procedure count");
			List<ushort> procedureOffsets = new(procedureCount);
			for (int procedureIndex = 0; procedureIndex < procedureCount; procedureIndex++)
			{
				procedureOffsets.Add(ReadWord(metadataBytes, ref cursor, mmePath, $"module {moduleIndex + 1} procedure offset {procedureIndex}"));
			}

			moduleHeaders.Add(new ModuleHeaderTableEntry(procedureOffsets));
		}

		if (cursor > endOffset)
		{
			throw new LinchpinException($"MME file '{mmePath}' overruns the declared module header table.");
		}

		return moduleHeaders;
	}

	private static List<ModuleImage> ParseModules(byte[] objectBytes, int codeEndOffset, IReadOnlyList<int> moduleOffsetPages, IReadOnlyList<ModuleHeaderTableEntry> moduleHeaders, string objPath)
	{
		List<ModuleImage> modules = new(moduleOffsetPages.Count);
		for (int moduleIndex = 0; moduleIndex < moduleOffsetPages.Count; moduleIndex++)
		{
			int moduleOffset = moduleOffsetPages[moduleIndex] * 0x100;
			int nextOffset = moduleIndex == moduleOffsetPages.Count - 1
				? codeEndOffset
				: moduleOffsetPages[moduleIndex + 1] * 0x100;
			int moduleLength = nextOffset - moduleOffset;

			if (moduleOffset < 0 || moduleLength <= 0 || nextOffset > objectBytes.Length)
			{
				throw new LinchpinException($"OBJ file '{objPath}' contains an invalid span for module {moduleIndex + 1}.");
			}

			List<ProcedureEntry> procedures = new(moduleHeaders[moduleIndex].ProcedureOffsets.Count);
			for (int procedureIndex = 0; procedureIndex < moduleHeaders[moduleIndex].ProcedureOffsets.Count; procedureIndex++)
			{
				int procedureOffset = moduleHeaders[moduleIndex].ProcedureOffsets[procedureIndex];
				if (procedureOffset >= moduleLength)
				{
					throw new LinchpinException($"OBJ file '{objPath}' places module {moduleIndex + 1} procedure {procedureIndex} outside its module span.");
				}

				procedures.Add(ParseProcedureEntry(objectBytes, new ModuleImage(moduleIndex + 1, moduleOffset, moduleLength, Array.Empty<ProcedureEntry>()), procedureOffset, objPath, procedureIndex));
			}

			modules.Add(new ModuleImage(moduleIndex + 1, moduleOffset, moduleLength, procedures));
		}

		return modules;
	}

	/// <summary>
	/// Parses a procedure entry (header and code offset) from the OBJ bytecode at the given offset within a module.
	/// </summary>
	/// <param name="objectBytes">Raw OBJ bytecode.</param>
	/// <param name="module">The module containing the procedure.</param>
	/// <param name="procedureOffset">Module-relative byte offset of the procedure header.</param>
	/// <param name="objPath">Path to the OBJ file (for error messages).</param>
	/// <param name="exportedProcedureIndex">
	/// The exported procedure index if the procedure appears in the module header table, or <c>null</c> for
	/// private (discovered) procedures.
	/// </param>
	/// <returns>The parsed procedure entry.</returns>
	internal static ProcedureEntry ParseProcedureEntry(byte[] objectBytes, ModuleImage module, int procedureOffset, string objPath, int? exportedProcedureIndex = null)
	{
		if (procedureOffset >= module.Length)
		{
			throw new LinchpinException($"OBJ file '{objPath}' places module {module.ModuleId} procedure offset 0x{procedureOffset:X4} outside its module span.");
		}

		ProcedureHeader header = ParseProcedureHeader(objectBytes, module.ObjectOffset, module.Length, procedureOffset, objPath, module.ModuleId, exportedProcedureIndex ?? -1);
		return new ProcedureEntry(exportedProcedureIndex, procedureOffset, procedureOffset + header.HeaderSize, header);
	}

	private static ProcedureHeader ParseProcedureHeader(byte[] objectBytes, int moduleOffset, int moduleLength, int procedureOffset, string objPath, int moduleId, int procedureIndex)
	{
		int absoluteOffset = moduleOffset + procedureOffset;
		int moduleEndOffset = moduleOffset + moduleLength;
		if (absoluteOffset >= moduleEndOffset)
		{
			throw new LinchpinException($"OBJ file '{objPath}' truncates the header for module {moduleId} procedure {procedureIndex}.");
		}

		int headerStart = absoluteOffset;
		byte headerByte = objectBytes[absoluteOffset++];
		int localCount = headerByte & 0x7F;
		bool hasInitializers = (headerByte & 0x80) != 0;
		List<ProcedureInitializer> initializers = new();

		if (hasInitializers)
		{
			while (true)
			{
				if (absoluteOffset >= moduleEndOffset)
				{
					throw new LinchpinException($"OBJ file '{objPath}' truncates the initializer list for module {moduleId} procedure {procedureIndex}.");
				}

				byte marker = objectBytes[absoluteOffset++];
				int localIndex = marker & 0x3F;
				bool valueIsByte = (marker & 0x40) != 0;
				bool isLast = (marker & 0x80) != 0;

				ushort value;
				if (valueIsByte)
				{
					if (absoluteOffset >= moduleEndOffset)
					{
						throw new LinchpinException($"OBJ file '{objPath}' truncates a byte initializer for module {moduleId} procedure {procedureIndex}.");
					}

					value = unchecked((ushort)(short)(sbyte)objectBytes[absoluteOffset++]);
				}
				else
				{
					if (absoluteOffset + 1 >= moduleEndOffset)
					{
						throw new LinchpinException($"OBJ file '{objPath}' truncates a word initializer for module {moduleId} procedure {procedureIndex}.");
					}

					value = (ushort)(objectBytes[absoluteOffset] | (objectBytes[absoluteOffset + 1] << 8));
					absoluteOffset += 2;
				}

				initializers.Add(new ProcedureInitializer(localIndex, value, valueIsByte));
				if (isLast)
				{
					break;
				}
			}
		}

		return new ProcedureHeader(localCount, absoluteOffset - headerStart, initializers);
	}

	/// <summary>
	/// Infers the initial RAM byte count from the raw header value, which may be expressed as
	/// either a byte count or a word count depending on the image.
	/// </summary>
	private static int InferInitialRamByteCount(ushort rawHeaderValue, int availableBytes, string mmePath)
	{
		int bytesInterpretation = rawHeaderValue;
		int wordsInterpretation = rawHeaderValue * 2;

		if (wordsInterpretation <= availableBytes)
		{
			return wordsInterpretation;
		}

		if (wordsInterpretation == availableBytes)
		{
			return wordsInterpretation;
		}

		if (bytesInterpretation == availableBytes)
		{
			return bytesInterpretation;
		}

		if (bytesInterpretation <= availableBytes && wordsInterpretation > availableBytes)
		{
			return bytesInterpretation;
		}

		if (wordsInterpretation <= availableBytes && bytesInterpretation > availableBytes)
		{
			return wordsInterpretation;
		}

		if (bytesInterpretation <= availableBytes)
		{
			return bytesInterpretation;
		}

		throw new LinchpinException($"MME file '{mmePath}' declares an initial RAM length that does not fit the file.");
	}

	private static string DescribeRamLengthUnits(ushort rawHeaderValue, int byteCount)
	{
		if (rawHeaderValue * 2 == byteCount)
		{
			return $"header 0x{rawHeaderValue:X4} interpreted as words";
		}

		return $"header 0x{rawHeaderValue:X4} interpreted as bytes";
	}

	private static ushort[] ReadWords(byte[] bytes, int offset, int count)
	{
		ushort[] words = new ushort[count];
		for (int index = 0; index < count; index++)
		{
			int wordOffset = offset + index * 2;
			words[index] = (ushort)(bytes[wordOffset] | (bytes[wordOffset + 1] << 8));
		}

		return words;
	}

	private static ushort ReadWord(byte[] bytes, ref int cursor, string path, string description)
	{
		if (cursor + 1 >= bytes.Length)
		{
			throw new LinchpinException($"File '{path}' truncates while reading {description}.");
		}

		ushort value = (ushort)(bytes[cursor] | (bytes[cursor + 1] << 8));
		cursor += 2;
		return value;
	}

	/// <summary>
	/// Rounds a byte offset up to the next 256-byte page boundary.
	/// </summary>
	private static int AlignToPage(int value)
	{
		return (value + 0xFF) & ~0xFF;
	}

	private sealed record ModuleHeaderTableEntry(IReadOnlyList<ushort> ProcedureOffsets);
}

/// <summary>
/// Represents a fully loaded Cornerstone VM image (metadata + bytecode).
/// </summary>
internal sealed record CornerstoneImage(
	string MmePath,
	string ObjPath,
	IReadOnlyList<ushort> HeaderWords,
	byte[] MetadataBytes,
	byte[] ObjectBytes,
	EntryPoint EntryPoint,
	int CodeEndOffset,
	int ModuleHeaderOffset,
	int ModuleHeaderLengthWords,
	int InitialRamOffset,
	byte[] InitialRamBytes,
	string InitialRamLengthUnits,
	GlobalLayout Globals,
	IReadOnlyList<ModuleImage> Modules);

/// <summary>
/// Identifies the entrypoint procedure by module ID and procedure index.
/// </summary>
internal sealed record EntryPoint(int ModuleId, int ProcedureIndex);

/// <summary>
/// Describes the global variable layout: program-wide globals and per-module globals with their initial values.
/// </summary>
internal sealed record GlobalLayout(
	int ProgramGlobalCount,
	IReadOnlyList<ushort> ModuleGlobalCounts,
	IReadOnlyList<ushort> ProgramGlobals,
	IReadOnlyList<IReadOnlyList<ushort>> ModuleGlobals);

/// <summary>
/// Represents one module within the OBJ file, including its byte range and procedure table.
/// </summary>
internal sealed record ModuleImage(int ModuleId, int ObjectOffset, int Length, IReadOnlyList<ProcedureEntry> Procedures);

/// <summary>
/// Describes a single procedure within a module: its location, header, and whether it is exported.
/// </summary>
internal sealed record ProcedureEntry(int? ExportedProcedureIndex, int StartOffset, int CodeOffset, ProcedureHeader Header)
{
	public bool IsExported => ExportedProcedureIndex.HasValue;
	public int ProcedureIndex => ExportedProcedureIndex ?? -1;
}

/// <summary>
/// Describes a procedure's header: local variable count, header byte size, and initializer list.
/// </summary>
internal sealed record ProcedureHeader(int LocalCount, int HeaderSize, IReadOnlyList<ProcedureInitializer> Initializers);

/// <summary>
/// A single local-variable initializer within a procedure header.
/// </summary>
internal sealed record ProcedureInitializer(int LocalIndex, ushort Value, bool IsByteEncoded);

/// <summary>
/// The top-level command that Linchpin should execute.
/// </summary>
internal enum LinchpinCommand
{
	Inspect,
	Run,
	EmitSource,
}

/// <summary>
/// Locates the repository root by searching upward for <c>linchpin.slnx</c>.
/// </summary>
internal static class RepositoryLocator
{
	public static string FindRepositoryRoot()
	{
		foreach (string candidate in GetSearchRoots())
		{
			string? current = Path.GetFullPath(candidate);
			while (!string.IsNullOrEmpty(current))
			{
				if (File.Exists(Path.Combine(current, "linchpin.slnx")))
				{
					return current;
				}

				current = Directory.GetParent(current)?.FullName;
			}
		}

		throw new LinchpinException("Could not locate the repository root containing linchpin.slnx.");
	}

	private static IEnumerable<string> GetSearchRoots()
	{
		yield return Environment.CurrentDirectory;
		yield return AppContext.BaseDirectory;
	}
}

/// <summary>
/// Exception type for all Linchpin-specific error conditions.
/// </summary>
internal sealed class LinchpinException : Exception
{
	public LinchpinException(string message)
		: base(message)
	{
	}
}