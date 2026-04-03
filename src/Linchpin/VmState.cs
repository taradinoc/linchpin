namespace Linchpin;

/// <summary>
/// Creates the initial VM state from a loaded Cornerstone image, setting up the entry point's
/// call frame, globals, and the two-segment RAM arena.
/// </summary>
internal static class VmBootstrapper
{
	private const int FullVmRamBytes = 0x20000;
	private const ushort FalseSentinel = 0x8001;
	private const int SegmentSizeBytes = 0x10000;
	private const int TupleStackReserveBytes = SegmentSizeBytes / 64;

	/// <summary>
	/// Builds the starting <see cref="VmState"/> from the image's entry point, initializing
	/// local variables, globals, and the RAM arena with its free-list headers.
	/// </summary>
	/// <param name="image">The loaded Cornerstone image.</param>
	/// <returns>A VM state ready for execution.</returns>
	public static VmState CreateInitialState(CornerstoneImage image)
	{
		ModuleImage module = image.Modules[image.EntryPoint.ModuleId - 1];
		ProcedureEntry procedure = module.Procedures[image.EntryPoint.ProcedureIndex];
		ushort[] locals = new ushort[procedure.Header.LocalCount];
		Array.Fill(locals, FalseSentinel);

		foreach (ProcedureInitializer initializer in procedure.Header.Initializers)
		{
			if (initializer.LocalIndex >= locals.Length)
			{
				throw new LinchpinException($"Procedure initializer targets local {initializer.LocalIndex}, but the entry procedure only declares {locals.Length} locals.");
			}

			locals[initializer.LocalIndex] = initializer.Value;
		}

		VmFrame frame = new(module.ModuleId, procedure.ProcedureIndex, procedure.StartOffset, procedure.CodeOffset, locals);
		ushort[] programGlobals = image.Globals.ProgramGlobals.ToArray();
		IReadOnlyList<ushort[]> moduleGlobals = image.Globals.ModuleGlobals.Select(values => values.ToArray()).ToArray();
		byte[] ramBytes = CreateInitialRam(image.InitialRamBytes);

		return new VmState(module.ModuleId, procedure.CodeOffset, ramBytes, programGlobals, moduleGlobals, Array.Empty<ushort>(), frame);
	}

	private static byte[] CreateInitialRam(byte[] initialRamBytes)
	{
		byte[] ramBytes = new byte[FullVmRamBytes];
		Array.Copy(initialRamBytes, ramBytes, initialRamBytes.Length);

		int lowArenaOffset = (initialRamBytes.Length + 1) & ~1;
		SeedArena(ramBytes, lowArenaOffset, SegmentSizeBytes);

		int highArenaOffset = SegmentSizeBytes;
		SeedArena(ramBytes, highArenaOffset, SegmentSizeBytes - TupleStackReserveBytes);

		return ramBytes;
	}

	/// <summary>
	/// Sets up a free-list header at the base of a RAM arena segment so the VM's
	/// allocation routines can find and manage free space.
	/// </summary>
	private static void SeedArena(byte[] ramBytes, int arenaByteOffset, int segmentExtentBytes)
	{
		if (arenaByteOffset + 10 > ramBytes.Length)
		{
			return;
		}

		int segmentBase = arenaByteOffset & ~0xFFFF;
		int intraSegmentOffset = arenaByteOffset - segmentBase;
		if (intraSegmentOffset + 10 > segmentExtentBytes)
		{
			return;
		}

		ushort arenaHandle = (ushort)((segmentBase == 0 ? 0 : 0x8000) | (intraSegmentOffset / 2));
		WriteWord(ramBytes, arenaByteOffset, 0x0000);
		WriteWord(ramBytes, arenaByteOffset + 2, (ushort)(arenaHandle + 3));
		WriteWord(ramBytes, arenaByteOffset + 6, (ushort)(arenaHandle + 3));
		WriteWord(ramBytes, arenaByteOffset + 8, (ushort)(segmentExtentBytes - (intraSegmentOffset + 8)));
	}

	private static void WriteWord(byte[] ramBytes, int offset, ushort value)
	{
		ramBytes[offset] = (byte)(value & 0xFF);
		ramBytes[offset + 1] = (byte)(value >> 8);
	}
}

/// <summary>
/// A snapshot of the VM's full mutable state: program counter, RAM, globals, evaluation stack,
/// and the current call frame.
/// </summary>
internal sealed record VmState(
	int CurrentModuleId,
	int ProgramCounter,
	byte[] RamBytes,
	ushort[] ProgramGlobals,
	IReadOnlyList<ushort[]> ModuleGlobals,
	IReadOnlyList<ushort> EvaluationStack,
	VmFrame CurrentFrame);

/// <summary>
/// A single activation record on the VM call stack, holding the return context and local variables.
/// </summary>
internal sealed record VmFrame(
	int ModuleId,
	int ProcedureIndex,
	int ProcedureStartOffset,
	int CodeOffset,
	ushort[] Locals);