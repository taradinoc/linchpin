/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Chisel;

internal static partial class Program
{
    /// <summary>
	/// Accumulates data directives for a single logically contiguous RAM block while parsing the source.
	/// Multiple consecutive <c>.string</c>, <c>.word</c>, or <c>.byte</c> directives with the same label
	/// are grouped into one block.
	/// </summary>
	private sealed class PendingRamBlock
	{
		/// <summary>Initializes a new <see cref="PendingRamBlock"/> starting at the given RAM address.</summary>
		/// <param name="label">The label that names this block.</param>
		/// <param name="lineNumber">The source line where this block was first declared.</param>
		/// <param name="wordAddress">The word address at which this block starts in RAM.</param>
		public PendingRamBlock(string label, int lineNumber, ushort wordAddress)
		{
			Label = label;
			LineNumber = lineNumber;
			WordAddress = wordAddress;
		}

		/// <summary>The label that names this RAM block.</summary>
		public string Label { get; }

		/// <summary>The source line where this block was first declared.</summary>
		public int LineNumber { get; }

		/// <summary>The word address at which this block starts in RAM.</summary>
		public ushort WordAddress { get; }

		/// <summary>The data directives that make up this block's content, in declaration order.</summary>
		public List<DataDirective> Directives { get; } = new();
	}
}
