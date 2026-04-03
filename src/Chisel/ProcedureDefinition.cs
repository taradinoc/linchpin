/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Chisel;

internal static partial class Program
{
    /// <summary>
	/// Represents a procedure declared in CAS source: its name, local variables, initializers, and body statements.
	/// </summary>
	private sealed class ProcedureDefinition
	{
		/// <summary>Initializes a new <see cref="ProcedureDefinition"/>.</summary>
		/// <param name="name">The procedure name.</param>
		/// <param name="localCount">The initial local-variable slot count (from legacy syntax), or 0 if <c>.local</c> directives are used.</param>
		/// <param name="lineNumber">The source line where the <c>.proc</c> directive appears.</param>
		/// <param name="usesLegacyLocalCountSyntax">Whether the legacy <c>.proc Name locals N</c> form was used.</param>
		public ProcedureDefinition(string name, int localCount, int lineNumber, bool usesLegacyLocalCountSyntax)
		{
			Name = name;
			LocalCount = localCount;
			LineNumber = lineNumber;
			UsesLegacyLocalCountSyntax = usesLegacyLocalCountSyntax;
		}

		/// <summary>The procedure name.</summary>
		public string Name { get; }

		/// <summary>The number of local variable slots allocated for this procedure.</summary>
		public int LocalCount { get; private set; }

		/// <summary>The source line where this procedure was declared.</summary>
		public int LineNumber { get; }

		/// <summary>
		/// <see langword="true"/> if the procedure was declared with the legacy <c>.proc Name locals N</c> syntax,
		/// in which case <c>.local</c> directives are not permitted.
		/// </summary>
		public bool UsesLegacyLocalCountSyntax { get; }

		/// <summary>The 0-based procedure-table index assigned during assembly, or -1 for private (unexported) procedures.</summary>
		public int ProcedureIndex { get; set; } = -1;

		/// <summary>Maps local variable names to their 0-based indices.</summary>
		public Dictionary<string, int> LocalNames { get; } = new(StringComparer.OrdinalIgnoreCase);

		/// <summary>The initializers for local variable slots, encoded in the procedure header.</summary>
		public List<LocalInitializer> Initializers { get; } = new();

		/// <summary>The statements making up the procedure body, in source order.</summary>
		public List<Statement> Statements { get; } = new();

		private HashSet<string> LocalLabelNames { get; } = new(StringComparer.OrdinalIgnoreCase);

		/// <summary>
		/// Declares a named local variable in this procedure, assigning it the next available slot index,
		/// and optionally registering an initializer for it.
		/// </summary>
		/// <param name="name">The local variable name.</param>
		/// <param name="initializerToken">The value token for the initializer, or <see langword="null"/> for no initializer.</param>
		/// <param name="lineNumber">The source line of the <c>.local</c> directive.</param>
		public void AddNamedLocal(string name, string? initializerToken, int lineNumber)
		{
			if (UsesLegacyLocalCountSyntax)
			{
				throw new AssemblerException($"Line {lineNumber}: .local cannot be mixed with legacy '.proc {Name} locals <count>' syntax.");
			}

			if (string.IsNullOrWhiteSpace(name))
			{
				throw new AssemblerException($"Line {lineNumber}: local name must not be empty.");
			}

			if (LocalNames.ContainsKey(name))
			{
				throw new AssemblerException($"Line {lineNumber}: duplicate local '{name}' in procedure '{Name}'.");
			}

			if (LocalCount >= 127)
			{
				throw new AssemblerException($"Line {lineNumber}: procedure '{Name}' cannot declare more than 127 locals.");
			}

			int localIndex = LocalCount;
			LocalNames[name] = localIndex;
			LocalCount++;

			if (initializerToken is not null)
			{
				AddInitializer(localIndex, initializerToken, lineNumber);
			}
		}

		/// <summary>Records a local code label name, raising an error if the same label is already defined in this procedure.</summary>
		/// <param name="name">The label name (without the trailing colon).</param>
		/// <param name="lineNumber">The source line where the label appears.</param>
		public void AddLocalLabel(string name, int lineNumber)
		{
			if (!LocalLabelNames.Add(name))
			{
				throw new AssemblerException($"Line {lineNumber}: duplicate local label '{name}' in procedure '{Name}'.");
			}
		}

		/// <summary>Adds an initializer for a local variable, identified by name or numeric index.</summary>
		/// <param name="localToken">The local variable name or a numeric index string.</param>
		/// <param name="initializerToken">The value token for the initializer.</param>
		/// <param name="lineNumber">The source line of the <c>.init</c> directive.</param>
		public void AddInitializer(string localToken, string initializerToken, int lineNumber)
		{
			int localIndex;
			if (TryParseInteger(localToken, out int numericIndex))
			{
				localIndex = numericIndex;
			}
			else if (!LocalNames.TryGetValue(localToken, out localIndex))
			{
				throw new AssemblerException($"Line {lineNumber}: unknown local '{localToken}' in procedure '{Name}'.");
			}

			AddInitializer(localIndex, initializerToken, lineNumber);
		}

		private void AddInitializer(int localIndex, string initializerToken, int lineNumber)
		{
			if (localIndex < 0 || localIndex >= LocalCount)
			{
				throw new AssemblerException($"Line {lineNumber}: local index {localIndex} is outside procedure '{Name}' local range 0..{Math.Max(LocalCount - 1, 0)}.");
			}

			if (localIndex > 63)
			{
				throw new AssemblerException($"Line {lineNumber}: local initializer target {localIndex} exceeds the encodable initializer range 0..63.");
			}

			Initializers.Add(new LocalInitializer(localIndex, initializerToken, lineNumber));
		}
	}
}
