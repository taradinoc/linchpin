/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Chisel;

internal static partial class Program
{
    /// <summary>
	/// Records the layout of an assembled module: the placement of each procedure and the final encoded bytes.
	/// </summary>
	private sealed class ModuleLayout
	{
		/// <summary>Initializes a new <see cref="ModuleLayout"/> for the given module and procedure layouts.</summary>
		/// <param name="module">The module definition that this layout describes.</param>
		/// <param name="procedures">The laid-out procedures within the module.</param>
		public ModuleLayout(ModuleDefinition module, IReadOnlyList<ProcedureLayout> procedures)
		{
			Module = module;
			Procedures = procedures;
		}

		/// <summary>The module definition that this layout describes.</summary>
		public ModuleDefinition Module { get; }

		/// <summary>The laid-out procedures in this module, in declaration order.</summary>
		public IReadOnlyList<ProcedureLayout> Procedures { get; }

		/// <summary>The final encoded bytes for this module, populated after encoding is complete.</summary>
		public byte[]? Bytes { get; set; }

		/// <summary>The byte offset of this module's code within the combined OBJ image.</summary>
		public int ObjectOffset { get; set; }
	}
}
