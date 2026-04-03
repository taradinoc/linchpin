namespace Linchpin.LP128Pack;

internal static class BundleFormat
{
	public const uint Magic = 0x31425343; // "CSB1" little-endian
	public const ushort VersionMajor = 1;
	public const ushort VersionMinor = 0;
	public const ushort HeaderSize = 72;
	public const ushort DirectoryEntrySize = 28;
	public const ushort TargetProfileC128Reu = 1;
	public const ushort CodePageSize = 0x100;
	public const ushort RamPageSize = 0x100;

	[Flags]
	internal enum BundleFlags : ushort
	{
		None = 0,
		LittleEndian = 1 << 0,
	}

	internal enum SectionKind : ushort
	{
		ImageSummary = 1,
		ModuleTable = 2,
		ExportProcedureTable = 3,
		ProcedureInitializerTable = 4,
		GlobalLayout = 5,
		InitialRam = 6,
		CodePages = 7,
		ReadOnlyDataPages = 8,
	}

	[Flags]
	internal enum SectionFlags : ushort
	{
		None = 0,
		MandatoryAtBoot = 1 << 0,
		Pageable = 1 << 1,
		ReadOnly = 1 << 2,
		HostOnly = 1 << 3,
	}

	internal sealed record SectionDefinition(
		SectionKind Kind,
		SectionFlags Flags,
		byte[] Data,
		uint Alignment = 0x100,
		ushort Codec = 0);
}