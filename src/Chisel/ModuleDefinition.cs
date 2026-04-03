/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Chisel;

internal static partial class Program
{
    /// <summary>
	/// Represents a module declared in CAS source: its name, assigned ID, exports, globals, and procedures.
	/// </summary>
	private sealed class ModuleDefinition
	{
		/// <summary>Initializes a new <see cref="ModuleDefinition"/> with the given name and source line.</summary>
		/// <param name="name">The module name as declared in the source.</param>
		/// <param name="lineNumber">The source line where the <c>.module</c> directive appears.</param>
		public ModuleDefinition(string name, int lineNumber)
		{
			Name = name;
			LineNumber = lineNumber;
		}

		/// <summary>The module name as declared in the source.</summary>
		public string Name { get; }

		/// <summary>The source line where this module was declared.</summary>
		public int LineNumber { get; }

		/// <summary>The 1-based module ID assigned during assembly.</summary>
		public int ModuleId { get; set; }

		/// <summary>The raw initializer-value tokens for module-global slots, in declaration order.</summary>
		public List<string> ModuleGlobals { get; } = new();

		/// <summary>Maps module-global slot names to their 0-based indices within <see cref="ModuleGlobals"/>.</summary>
		public Dictionary<string, int> ModuleGlobalNames { get; } = new(StringComparer.OrdinalIgnoreCase);

		/// <summary>The source line where module globals were first declared, or <see langword="null"/> if none were declared.</summary>
		public int? ModuleGlobalsLine { get; set; }

		/// <summary>The export declarations that map procedure names to their 0-based procedure-table indices.</summary>
		public List<ProcedureExportDefinition> Exports { get; } = new();

		/// <summary>The procedures defined within this module, in declaration order.</summary>
		public List<ProcedureDefinition> Procedures { get; } = new();
	}
}
