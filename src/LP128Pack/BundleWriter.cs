using System.Buffers.Binary;

namespace Linchpin.LP128Pack;

internal static class BundleWriter
{
	public static void Write(CornerstoneImage image, string outputPath)
	{
		List<BundleFormat.SectionDefinition> sections = BuildSections(image);
		using FileStream stream = File.Create(outputPath);
		using BinaryWriter writer = new(stream);

		uint directoryOffset = BundleFormat.HeaderSize;
		uint sectionCount = checked((uint)sections.Count);
		uint firstSectionOffset = Align(directoryOffset + sectionCount * BundleFormat.DirectoryEntrySize, 0x100);
		uint nextOffset = firstSectionOffset;

		List<(BundleFormat.SectionDefinition Section, uint Offset)> layout = new(sections.Count);
		foreach (BundleFormat.SectionDefinition section in sections)
		{
			nextOffset = Align(nextOffset, section.Alignment);
			layout.Add((section, nextOffset));
			nextOffset = checked(nextOffset + (uint)section.Data.Length);
		}

		WriteHeader(writer, image, layout, directoryOffset);
		WriteDirectory(writer, layout);

		long currentPosition = stream.Position;
		foreach ((BundleFormat.SectionDefinition section, uint offset) in layout)
		{
			WritePadding(writer, checked((int)(offset - currentPosition)));
			writer.Write(section.Data);
			currentPosition = stream.Position;
		}
	}

	private static void WriteHeader(BinaryWriter writer, CornerstoneImage image, IReadOnlyList<(BundleFormat.SectionDefinition Section, uint Offset)> layout, uint directoryOffset)
	{
		uint totalModuleGlobalCount = checked((uint)image.Globals.ModuleGlobalCounts.Sum(static count => count));
		writer.Write(BundleFormat.Magic);
		writer.Write(BundleFormat.VersionMajor);
		writer.Write(BundleFormat.VersionMinor);
		writer.Write(BundleFormat.HeaderSize);
		writer.Write(BundleFormat.DirectoryEntrySize);
		writer.Write(checked((ushort)layout.Count));
		writer.Write((ushort)BundleFormat.BundleFlags.LittleEndian);
		writer.Write(BundleFormat.TargetProfileC128Reu);
		writer.Write(BundleFormat.CodePageSize);
		writer.Write(BundleFormat.RamPageSize);
		writer.Write(checked((ushort)image.EntryPoint.ModuleId));
		writer.Write(checked((ushort)image.EntryPoint.ProcedureIndex));
		writer.Write(checked((ushort)image.Modules.Count));
		writer.Write(checked((ushort)image.Globals.ProgramGlobalCount));
		writer.Write(totalModuleGlobalCount);
		writer.Write(checked((uint)image.InitialRamBytes.Length));
		writer.Write(0u);
		writer.Write(directoryOffset);
		writer.Write(0u);
		writer.Write(0u);
		writer.Write(checked((uint)image.CodeEndOffset));
		writer.Write(checked((uint)image.ModuleHeaderOffset));
		writer.Write(checked((uint)image.ModuleHeaderLengthWords));
		writer.Write(checked((uint)image.InitialRamOffset));

		int written = 70;
		WritePadding(writer, BundleFormat.HeaderSize - written);
	}

	private static void WriteDirectory(BinaryWriter writer, IReadOnlyList<(BundleFormat.SectionDefinition Section, uint Offset)> layout)
	{
		foreach ((BundleFormat.SectionDefinition section, uint offset) in layout)
		{
			writer.Write((ushort)section.Kind);
			writer.Write((ushort)section.Flags);
			writer.Write(section.Codec);
			writer.Write((ushort)0);
			writer.Write(offset);
			writer.Write(checked((uint)section.Data.Length));
			writer.Write(checked((uint)section.Data.Length));
			writer.Write(section.Alignment);
			writer.Write(0u);
		}
	}

	private static List<BundleFormat.SectionDefinition> BuildSections(CornerstoneImage image)
	{
		List<BundleFormat.SectionDefinition> sections = [];
		sections.Add(new(BundleFormat.SectionKind.ImageSummary, BundleFormat.SectionFlags.MandatoryAtBoot, BuildImageSummary(image)));
		sections.Add(new(BundleFormat.SectionKind.ModuleTable, BundleFormat.SectionFlags.MandatoryAtBoot, BuildModuleTable(image)));
		sections.Add(new(BundleFormat.SectionKind.ExportProcedureTable, BundleFormat.SectionFlags.MandatoryAtBoot, BuildExportProcedureTable(image, out IReadOnlyList<ProcedureInitializer> initializers)));
		sections.Add(new(BundleFormat.SectionKind.ProcedureInitializerTable, BundleFormat.SectionFlags.MandatoryAtBoot, BuildInitializerTable(initializers)));
		sections.Add(new(BundleFormat.SectionKind.GlobalLayout, BundleFormat.SectionFlags.MandatoryAtBoot, BuildGlobalLayout(image)));
		sections.Add(new(BundleFormat.SectionKind.InitialRam, BundleFormat.SectionFlags.MandatoryAtBoot, image.InitialRamBytes.ToArray()));
		sections.Add(new(BundleFormat.SectionKind.CodePages, BundleFormat.SectionFlags.ReadOnly | BundleFormat.SectionFlags.Pageable, image.ObjectBytes[..image.CodeEndOffset].ToArray()));

		// Read-only data pages: everything after CodeEndOffset in the OBJ file.
		if (image.CodeEndOffset < image.ObjectBytes.Length)
		{
			sections.Add(new(BundleFormat.SectionKind.ReadOnlyDataPages, BundleFormat.SectionFlags.ReadOnly | BundleFormat.SectionFlags.Pageable, image.ObjectBytes[image.CodeEndOffset..].ToArray()));
		}

		return sections;
	}

	private static byte[] BuildImageSummary(CornerstoneImage image)
	{
		byte[] buffer = new byte[8 + image.HeaderWords.Count * 2];
		BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(0, 4), checked((uint)image.CodeEndOffset));
		BinaryPrimitives.WriteUInt16LittleEndian(buffer.AsSpan(4, 2), checked((ushort)image.ModuleHeaderOffset));
		BinaryPrimitives.WriteUInt16LittleEndian(buffer.AsSpan(6, 2), checked((ushort)image.InitialRamOffset));
		for (int index = 0; index < image.HeaderWords.Count; index++)
		{
			BinaryPrimitives.WriteUInt16LittleEndian(buffer.AsSpan(8 + index * 2, 2), image.HeaderWords[index]);
		}

		return buffer;
	}

	private static byte[] BuildModuleTable(CornerstoneImage image)
	{
		const int recordSize = 24;
		byte[] buffer = new byte[image.Modules.Count * recordSize];
		int exportStartIndex = 0;
		for (int index = 0; index < image.Modules.Count; index++)
		{
			ModuleImage module = image.Modules[index];
			Span<byte> record = buffer.AsSpan(index * recordSize, recordSize);
			BinaryPrimitives.WriteUInt16LittleEndian(record[0..2], checked((ushort)module.ModuleId));
			BinaryPrimitives.WriteUInt16LittleEndian(record[2..4], checked((ushort)module.Procedures.Count));
			BinaryPrimitives.WriteUInt16LittleEndian(record[4..6], checked((ushort)exportStartIndex));
			BinaryPrimitives.WriteUInt16LittleEndian(record[6..8], 0);
			BinaryPrimitives.WriteUInt32LittleEndian(record[8..12], checked((uint)module.ObjectOffset));
			BinaryPrimitives.WriteUInt32LittleEndian(record[12..16], checked((uint)module.Length));
			BinaryPrimitives.WriteUInt32LittleEndian(record[16..20], checked((uint)(module.ObjectOffset / BundleFormat.CodePageSize)));
			BinaryPrimitives.WriteUInt32LittleEndian(record[20..24], checked((uint)((module.Length + BundleFormat.CodePageSize - 1) / BundleFormat.CodePageSize)));
			exportStartIndex += module.Procedures.Count;
		}

		return buffer;
	}

	private static byte[] BuildExportProcedureTable(CornerstoneImage image, out IReadOnlyList<ProcedureInitializer> flattenedInitializers)
	{
		const int recordSize = 24;
		List<ProcedureInitializer> initializers = [];
		int exportCount = image.Modules.Sum(static module => module.Procedures.Count);
		byte[] buffer = new byte[exportCount * recordSize];
		int recordIndex = 0;

		foreach (ModuleImage module in image.Modules)
		{
			for (int procedureIndex = 0; procedureIndex < module.Procedures.Count; procedureIndex++)
			{
				ProcedureEntry procedure = module.Procedures[procedureIndex];
				int upperBound = procedureIndex + 1 < module.Procedures.Count
					? module.Procedures[procedureIndex + 1].StartOffset
					: module.Length;
				int initializerStartIndex = initializers.Count;
				initializers.AddRange(procedure.Header.Initializers);

				Span<byte> record = buffer.AsSpan(recordIndex * recordSize, recordSize);
				BinaryPrimitives.WriteUInt16LittleEndian(record[0..2], checked((ushort)module.ModuleId));
				BinaryPrimitives.WriteUInt16LittleEndian(record[2..4], checked((ushort)(procedure.ExportedProcedureIndex ?? 0xFFFF)));
				BinaryPrimitives.WriteUInt16LittleEndian(record[4..6], checked((ushort)procedure.Header.LocalCount));
				BinaryPrimitives.WriteUInt16LittleEndian(record[6..8], checked((ushort)procedure.Header.HeaderSize));
				BinaryPrimitives.WriteUInt16LittleEndian(record[8..10], checked((ushort)initializerStartIndex));
				BinaryPrimitives.WriteUInt16LittleEndian(record[10..12], checked((ushort)procedure.Header.Initializers.Count));
				BinaryPrimitives.WriteUInt16LittleEndian(record[12..14], checked((ushort)procedure.StartOffset));
				BinaryPrimitives.WriteUInt16LittleEndian(record[14..16], checked((ushort)procedure.CodeOffset));
				BinaryPrimitives.WriteUInt16LittleEndian(record[16..18], checked((ushort)upperBound));
				BinaryPrimitives.WriteUInt16LittleEndian(record[18..20], 0);
				BinaryPrimitives.WriteUInt32LittleEndian(record[20..24], 0);
				recordIndex++;
			}
		}

		flattenedInitializers = initializers;
		return buffer;
	}

	private static byte[] BuildInitializerTable(IReadOnlyList<ProcedureInitializer> initializers)
	{
		const int recordSize = 8;
		byte[] buffer = new byte[initializers.Count * recordSize];
		for (int index = 0; index < initializers.Count; index++)
		{
			ProcedureInitializer initializer = initializers[index];
			Span<byte> record = buffer.AsSpan(index * recordSize, recordSize);
			record[0] = checked((byte)initializer.LocalIndex);
			record[1] = initializer.IsByteEncoded ? (byte)1 : (byte)0;
			BinaryPrimitives.WriteUInt16LittleEndian(record[2..4], initializer.Value);
			BinaryPrimitives.WriteUInt16LittleEndian(record[4..6], 0);
			BinaryPrimitives.WriteUInt16LittleEndian(record[6..8], 0);
		}

		return buffer;
	}

	private static byte[] BuildGlobalLayout(CornerstoneImage image)
	{
		using MemoryStream stream = new();
		using BinaryWriter writer = new(stream);
		writer.Write(checked((ushort)image.Modules.Count));
		writer.Write(checked((ushort)image.Globals.ProgramGlobalCount));
		foreach (ushort count in image.Globals.ModuleGlobalCounts)
		{
			writer.Write(count);
		}

		foreach (ushort value in image.Globals.ProgramGlobals)
		{
			writer.Write(value);
		}

		foreach (IReadOnlyList<ushort> moduleGlobals in image.Globals.ModuleGlobals)
		{
			foreach (ushort value in moduleGlobals)
			{
				writer.Write(value);
			}
		}

		return stream.ToArray();
	}

	private static uint Align(uint value, uint alignment)
	{
		if (alignment == 0)
		{
			return value;
		}

		uint mask = alignment - 1;
		return (value + mask) & ~mask;
	}

	private static void WritePadding(BinaryWriter writer, int byteCount)
	{
		for (int index = 0; index < byteCount; index++)
		{
			writer.Write((byte)0);
		}
	}
}