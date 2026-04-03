/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.IO.Compression;
using System.Text.Json;

namespace Chisel;

internal static partial class Program
{
    /// <summary>
	/// Loads and indexes the MME instruction grammar, enabling lookup of opcodes by mnemonic.
	/// The grammar describes each instruction's opcode byte(s) and operand schema.
	/// </summary>
	private sealed class GrammarCatalog
	{
		private readonly Dictionary<string, OpcodeInfo> mnemonics;

		private GrammarCatalog(Dictionary<string, OpcodeInfo> mnemonics)
		{
			this.mnemonics = mnemonics;
		}

		/// <summary>
		/// Loads the instruction grammar from a JSON file, or from the embedded compressed resource if <paramref name="path"/> is <see langword="null"/>.
		/// </summary>
		/// <param name="path">Path to an external grammar JSON file, or <see langword="null"/> to use the embedded grammar.</param>
		/// <returns>A fully indexed <see cref="GrammarCatalog"/> ready for mnemonic lookup.</returns>
		public static GrammarCatalog Load(string? path)
		{
			using Stream stream = OpenGrammarStream(path);
			using JsonDocument document = JsonDocument.Parse(stream);
			Dictionary<string, OpcodeInfo> mnemonics = new(StringComparer.OrdinalIgnoreCase);

			foreach (JsonElement entry in document.RootElement.GetProperty("primary_opcode_table").EnumerateArray())
			{
				string form = entry.GetProperty("form").GetString() ?? string.Empty;
				if (form == "extension_prefix")
				{
					continue;
				}

				string mnemonic = entry.GetProperty("mnemonic").GetString() ?? throw new AssemblerException("Malformed grammar entry.");
				byte opcode = ParseHexByte(entry.GetProperty("opcode").GetString()!);
				List<GrammarOperand> operands = ParseOperands(entry.GetProperty("operands"));
				mnemonics[mnemonic] = new OpcodeInfo(mnemonic, opcode, 0, false, mnemonic.Equals("NEXTB", StringComparison.OrdinalIgnoreCase), operands);
			}

			foreach (JsonElement entry in document.RootElement.GetProperty("extended_opcode_table").EnumerateArray())
			{
				string mnemonic = entry.GetProperty("mnemonic").GetString() ?? throw new AssemblerException("Malformed grammar entry.");
				byte extOpcode = ParseHexByte(entry.GetProperty("opcode").GetString()!);
				List<GrammarOperand> operands = ParseOperands(entry.GetProperty("operands"));
				mnemonics[mnemonic] = new OpcodeInfo(mnemonic, 0x5F, extOpcode, true, false, operands);
			}

			return new GrammarCatalog(mnemonics);
		}

		/// <summary>Opens the grammar JSON stream from a file path, or from the embedded compressed resource if no path is given.</summary>
		/// <param name="path">Path to an external grammar JSON file, or <see langword="null"/> to use the embedded grammar.</param>
		/// <returns>A readable stream containing the grammar JSON.</returns>
		private static Stream OpenGrammarStream(string? path)
		{
			if (path is not null)
			{
				if (!File.Exists(path))
				{
					throw new AssemblerException($"Grammar file '{path}' does not exist.");
				}

				return File.OpenRead(path);
			}

			Stream? resource = typeof(GrammarCatalog).Assembly
				.GetManifestResourceStream("Chisel.cornerstone_instruction_grammar.json.gz");
			if (resource is null)
			{
				throw new AssemblerException("Embedded grammar resource not found.");
			}

			return new GZipStream(resource, CompressionMode.Decompress);
		}

		/// <summary>Looks up an opcode by mnemonic name, throwing an error if it is not found.</summary>
		/// <param name="mnemonic">The canonical mnemonic to look up.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <returns>The <see cref="OpcodeInfo"/> for the given mnemonic.</returns>
		public OpcodeInfo GetByMnemonic(string mnemonic, int lineNumber)
		{
			if (!mnemonics.TryGetValue(mnemonic, out OpcodeInfo? opcode))
			{
				throw new AssemblerException($"Line {lineNumber}: unknown mnemonic '{mnemonic}'.");
			}

			return opcode;
		}

		/// <summary>Parses the operand list from a grammar entry element.</summary>
		/// <param name="element">The JSON element containing the operands array.</param>
		/// <returns>A list of <see cref="GrammarOperand"/> describing each operand position.</returns>
		private static List<GrammarOperand> ParseOperands(JsonElement element)
		{
			List<GrammarOperand> operands = new();
			foreach (JsonElement operand in element.EnumerateArray())
			{
				operands.Add(new GrammarOperand(
					operand.GetProperty("name").GetString() ?? string.Empty,
					operand.GetProperty("kind").GetString() ?? string.Empty));
			}

			return operands;
		}
	}
}
