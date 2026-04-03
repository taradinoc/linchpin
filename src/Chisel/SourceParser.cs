/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Text;

namespace Chisel;

internal static partial class Program
{
    /// <summary>
	/// Parses CAS assembly source files (including any transitively included files) into an <see cref="AssemblySource"/>.
	/// </summary>
	private sealed class SourceParser
	{
		private readonly List<ModuleDefinition> modules = new();
		private readonly List<RamDirective> ramDirectives = new();
		private readonly List<ObjDataDirective> objDataDirectives = new();
		private readonly List<string> programGlobals = new();
		private readonly Dictionary<string, int> programGlobalNames = new(StringComparer.OrdinalIgnoreCase);
		private readonly Dictionary<string, ConstantDefinition> constants = new(StringComparer.OrdinalIgnoreCase);
		private readonly Stack<string> includeStack = new();

		private ModuleDefinition? currentModule;
		private ProcedureDefinition? currentProcedure;
		private string? currentRamLabel;
		private string? entrySymbol;
		private int entryLine;
		private int? programGlobalsLine;

		/// <summary>Parses a CAS source file (and all files it includes) and returns the complete <see cref="AssemblySource"/>.</summary>
		/// <param name="path">Path to the root CAS source file.</param>
		/// <returns>An <see cref="AssemblySource"/> containing all modules, directives, and declarations from the source.</returns>
		public static AssemblySource Parse(string path)
		{
			SourceParser parser = new();
			string fullPath = Path.GetFullPath(path);
			parser.ParseFile(fullPath);
			return parser.Build(fullPath);
		}

		/// <summary>Reads and parses a single source file, pushing it onto the include stack to detect circular includes.</summary>
		/// <param name="path">The absolute path of the file to parse.</param>
		private void ParseFile(string path)
		{
			if (!File.Exists(path))
			{
				throw new AssemblerException($"Input file '{path}' does not exist.");
			}

			if (includeStack.Contains(path, StringComparer.OrdinalIgnoreCase))
			{
				throw new AssemblerException($"Circular .include detected for '{path}'.");
			}

			includeStack.Push(path);
			try
			{
				string[] lines = File.ReadAllLines(path);
				for (int lineNumber = 1; lineNumber <= lines.Length; lineNumber++)
				{
					ParseLine(lines[lineNumber - 1], lineNumber, path);
				}
			}
			finally
			{
				includeStack.Pop();
			}
		}

		/// <summary>Validates the accumulated parse state and returns the final <see cref="AssemblySource"/>.</summary>
		/// <param name="path">The resolved absolute path of the root source file.</param>
		/// <returns>The completed <see cref="AssemblySource"/>.</returns>
		private AssemblySource Build(string path)
		{
			if (currentProcedure is not null)
			{
				throw new AssemblerException($"Line {currentProcedure.LineNumber}: procedure '{currentProcedure.Name}' is missing .endproc.");
			}

			if (entrySymbol is null)
			{
				throw new AssemblerException("Source does not declare an .entry directive.");
			}

			return new AssemblySource(path, modules, ramDirectives, objDataDirectives, programGlobals, programGlobalNames, constants, entrySymbol, entryLine, programGlobalsLine);
		}

		/// <summary>Parses a single source line: labels, directives, constant definitions, and instructions.</summary>
		/// <param name="line">The raw source line text.</param>
		/// <param name="lineNumber">The 1-based line number within <paramref name="currentPath"/>.</param>
		/// <param name="currentPath">The absolute path of the file being parsed, used to resolve relative includes.</param>
		private void ParseLine(string line, int lineNumber, string currentPath)
		{
			List<string> tokens = Tokenize(line);
			if (tokens.Count == 0)
			{
				return;
			}

			if (tokens[0].EndsWith("::", StringComparison.Ordinal))
			{
				if (currentProcedure is not null)
				{
					throw new AssemblerException($"Line {lineNumber}: global labels are only valid outside procedures.");
				}

				string labelName = tokens[0][..^2];
				if (string.IsNullOrWhiteSpace(labelName))
				{
					throw new AssemblerException($"Line {lineNumber}: malformed global label.");
				}

				currentRamLabel = labelName;
				tokens = tokens.Skip(1).ToList();
				if (tokens.Count == 0)
				{
					return;
				}
			}

			if (tokens[0].EndsWith(':'))
			{
				if (currentProcedure is null)
				{
					throw new AssemblerException($"Line {lineNumber}: labels are only valid inside procedures.");
				}

				string labelName = tokens[0][..^1];
				if (string.IsNullOrWhiteSpace(labelName))
				{
					throw new AssemblerException($"Line {lineNumber}: malformed label.");
				}

				currentProcedure.AddLocalLabel(labelName, lineNumber);
				currentProcedure.Statements.Add(new LabelStatement(labelName, lineNumber));
				tokens = tokens.Skip(1).ToList();
				if (tokens.Count == 0)
				{
					return;
				}
			}

			if (tokens[0].StartsWith(".", StringComparison.Ordinal))
			{
				ParseDirective(tokens, lineNumber, currentPath);
				return;
			}

			if (TryParseConstantDefinition(tokens, lineNumber, out ConstantDefinition? constant) && constant is not null)
			{
				if (currentRamLabel is not null)
				{
					throw new AssemblerException($"Line {lineNumber}: constant definitions cannot appear after a global RAM label.");
				}

				if (currentProcedure is not null)
				{
					throw new AssemblerException($"Line {lineNumber}: constant definitions are not valid inside procedure '{currentProcedure.Name}'.");
				}

				RegisterConstant(constant);
				return;
			}

			if (currentProcedure is null)
			{
				throw new AssemblerException($"Line {lineNumber}: instructions may only appear inside procedures.");
			}

			string mnemonic = tokens[0].ToUpperInvariant();
			List<string> operands = ParseCommaSeparated(tokens.Skip(1).ToList(), lineNumber);
			currentProcedure.Statements.Add(new InstructionDefinition(mnemonic, operands, lineNumber));
		}

		/// <summary>Dispatches a directive (tokens beginning with <c>.</c>) to its specific handler.</summary>
		/// <param name="tokens">The tokenized directive line, including the directive keyword as the first token.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <param name="currentPath">The absolute path of the current file, used to resolve <c>.include</c> paths.</param>
		private void ParseDirective(IReadOnlyList<string> tokens, int lineNumber, string currentPath)
		{
			string directive = tokens[0].ToLowerInvariant();
			if (directive is not ".string" and not ".word" and not ".words" and not ".byte" and not ".bytes")
			{
				currentRamLabel = null;
			}

			switch (directive)
			{
				case ".include":
					RequireProcedureClosed(".include", lineNumber);
					if (tokens.Count != 2 || !IsQuotedString(tokens[1]))
					{
						throw new AssemblerException($"Line {lineNumber}: .include expects a quoted relative or absolute path.");
					}

					string includePath = UnescapeString(tokens[1]);
					string resolvedPath = Path.IsPathRooted(includePath)
						? Path.GetFullPath(includePath)
						: Path.GetFullPath(Path.Combine(Path.GetDirectoryName(currentPath) ?? string.Empty, includePath));
					ParseFile(resolvedPath);
					break;

				case ".module":
					RequireProcedureClosed(".module", lineNumber);
					if (tokens.Count != 2)
					{
						throw new AssemblerException($"Line {lineNumber}: .module expects a single name.");
					}

					if (modules.Any(module => module.Name.Equals(tokens[1], StringComparison.OrdinalIgnoreCase)))
					{
						throw new AssemblerException($"Line {lineNumber}: duplicate module '{tokens[1]}'.");
					}

					currentModule = new ModuleDefinition(tokens[1], lineNumber);
					modules.Add(currentModule);
					break;

				case ".module_globals":
					if (currentModule is null)
					{
						throw new AssemblerException($"Line {lineNumber}: .module_globals must appear after .module.");
					}

					currentModule.ModuleGlobals.AddRange(ParseCommaSeparated(tokens.Skip(1).ToList(), lineNumber));
					currentModule.ModuleGlobalsLine ??= lineNumber;
					break;

					case ".global":
						RequireProcedureClosed(".global", lineNumber);
						List<NamedSlotDeclaration> globalDeclarations = ParseNamedSlotDeclarations(tokens.Skip(1).ToList(), lineNumber, requireInitializer: true);
						if (currentModule is null)
						{
							foreach (NamedSlotDeclaration declaration in globalDeclarations)
							{
								AddNamedSlot(programGlobals, programGlobalNames, declaration, lineNumber, "program global");
							}

							programGlobalsLine ??= lineNumber;
						}
						else
						{
							foreach (NamedSlotDeclaration declaration in globalDeclarations)
							{
								AddNamedSlot(currentModule.ModuleGlobals, currentModule.ModuleGlobalNames, declaration, lineNumber, $"module global in module '{currentModule.Name}'");
							}

							currentModule.ModuleGlobalsLine ??= lineNumber;
						}

						break;

				case ".export":
					if (currentModule is null)
					{
						throw new AssemblerException($"Line {lineNumber}: .export must appear inside a module.");
					}

					if (currentProcedure is not null)
					{
						throw new AssemblerException($"Line {lineNumber}: .export is not valid inside procedure '{currentProcedure.Name}'.");
					}

					List<string> exportOperands = ParseCommaSeparated(tokens.Skip(1).ToList(), lineNumber);
					if (exportOperands.Count != 2)
					{
						throw new AssemblerException($"Line {lineNumber}: .export expects '<procedure>, <index>'.");
					}

					currentModule.Exports.Add(new ProcedureExportDefinition(
						exportOperands[0],
						EvaluateNumericLiteral(exportOperands[1], lineNumber, 0, 255),
						lineNumber));
					break;

				case ".program_globals":
					RequireProcedureClosed(".program_globals", lineNumber);
					programGlobals.AddRange(ParseCommaSeparated(tokens.Skip(1).ToList(), lineNumber));
					programGlobalsLine ??= lineNumber;
					break;

				case ".proc":
					if (currentModule is null)
					{
						throw new AssemblerException($"Line {lineNumber}: .proc must appear inside a module.");
					}

					if (currentProcedure is not null)
					{
						throw new AssemblerException($"Line {lineNumber}: nested procedures are not allowed.");
					}

						if (tokens.Count != 2 && (tokens.Count != 4 || !tokens[2].Equals("locals", StringComparison.OrdinalIgnoreCase)))
					{
							throw new AssemblerException($"Line {lineNumber}: expected '.proc <name>' or '.proc <name> locals <count>'.");
					}

					if (currentModule.Procedures.Any(procedure => procedure.Name.Equals(tokens[1], StringComparison.OrdinalIgnoreCase)))
					{
						throw new AssemblerException($"Line {lineNumber}: duplicate procedure '{tokens[1]}' in module '{currentModule.Name}'.");
					}

						int localCount = tokens.Count == 4
							? EvaluateNumericLiteral(tokens[3], lineNumber, 0, 127)
							: 0;
						bool usesLegacyLocalCountSyntax = tokens.Count == 4;
						currentProcedure = new ProcedureDefinition(tokens[1], localCount, lineNumber, usesLegacyLocalCountSyntax);
					currentModule.Procedures.Add(currentProcedure);
					break;

					case ".local":
						if (currentProcedure is null)
						{
							throw new AssemblerException($"Line {lineNumber}: .local must appear inside a procedure.");
						}

						foreach (NamedSlotDeclaration declaration in ParseNamedSlotDeclarations(tokens.Skip(1).ToList(), lineNumber, requireInitializer: false))
						{
							currentProcedure.AddNamedLocal(declaration.Name, declaration.ValueToken, lineNumber);
						}

						break;

				case ".init":
					if (currentProcedure is null)
					{
						throw new AssemblerException($"Line {lineNumber}: .init must appear inside a procedure.");
					}

					List<string> initOperands = ParseCommaSeparated(tokens.Skip(1).ToList(), lineNumber);
					if (initOperands.Count != 2)
					{
						throw new AssemblerException($"Line {lineNumber}: .init expects '<local>, <value>'.");
					}

					currentProcedure.AddInitializer(initOperands[0], initOperands[1], lineNumber);
					break;

				case ".endproc":
					if (currentProcedure is null)
					{
						throw new AssemblerException($"Line {lineNumber}: .endproc appears outside a procedure.");
					}

					currentProcedure = null;
					break;

				case ".entry":
					RequireProcedureClosed(".entry", lineNumber);
					if (tokens.Count != 2)
					{
						throw new AssemblerException($"Line {lineNumber}: .entry expects a procedure symbol.");
					}

					entrySymbol = tokens[1];
					entryLine = lineNumber;
					break;

				case ".ramorg":
					RequireProcedureClosed(".ramorg", lineNumber);
					if (tokens.Count != 2)
					{
						throw new AssemblerException($"Line {lineNumber}: .ramorg expects a single word address.");
					}

					ramDirectives.Add(new RamOriginDirective(tokens[1], lineNumber));
					break;

				case ".string":
					RequireProcedureClosed(".string", lineNumber);
					ParseDataDirective(tokens, lineNumber, DataDirectiveKind.String);
					break;

				case ".objstring":
					RequireProcedureClosed(".objstring", lineNumber);
					ParseObjStringDirective(tokens, lineNumber);
					break;

				case ".objpacked":
					RequireProcedureClosed(".objpacked", lineNumber);
					ParseObjPackedDirective(tokens, lineNumber);
					break;

				case ".word":
				case ".words":
					RequireProcedureClosed(tokens[0], lineNumber);
					ParseDataDirective(tokens, lineNumber, DataDirectiveKind.Words);
					break;

				case ".byte":
				case ".bytes":
					RequireProcedureClosed(tokens[0], lineNumber);
					ParseDataDirective(tokens, lineNumber, DataDirectiveKind.Bytes);
					break;

				default:
					throw new AssemblerException($"Line {lineNumber}: unknown directive '{tokens[0]}'.");
			}
		}

		/// <summary>Parses a <c>.string</c>, <c>.word</c>, <c>.words</c>, <c>.byte</c>, or <c>.bytes</c> directive into a <see cref="DataDirective"/>.</summary>
		/// <param name="tokens">The tokenized directive line.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <param name="kind">The kind of data the directive declares.</param>
		private void ParseDataDirective(IReadOnlyList<string> tokens, int lineNumber, DataDirectiveKind kind)
		{
			if (currentRamLabel is not null)
			{
				if (kind == DataDirectiveKind.String)
				{
					if (tokens.Count != 2 || !IsQuotedString(tokens[1]))
					{
						throw new AssemblerException($"Line {lineNumber}: .string expects a quoted string after a global label.");
					}

					ramDirectives.Add(new DataDirective(kind, currentRamLabel, Array.Empty<string>(), UnescapeString(tokens[1]), lineNumber));
					return;
				}

				List<string> labeledOperands = ParseCommaSeparated(tokens.Skip(1).ToList(), lineNumber);
				if (labeledOperands.Count == 0)
				{
					throw new AssemblerException($"Line {lineNumber}: {tokens[0]} requires at least one value.");
				}

				ramDirectives.Add(new DataDirective(kind, currentRamLabel, labeledOperands, null, lineNumber));
				return;
			}

			if (tokens.Count < 4 || tokens[2] != ",")
			{
				throw new AssemblerException($"Line {lineNumber}: {tokens[0]} requires a preceding global label or legacy '<label>, ...' syntax.");
			}

			string label = tokens[1];
			if (kind == DataDirectiveKind.String)
			{
				if (tokens.Count != 4 || !IsQuotedString(tokens[3]))
				{
					throw new AssemblerException($"Line {lineNumber}: .string expects '<label>, " + '"' + "text" + '"' + "'.");
				}

				ramDirectives.Add(new DataDirective(kind, label, Array.Empty<string>(), UnescapeString(tokens[3]), lineNumber));
				return;
			}

			List<string> operands = ParseCommaSeparated(tokens.Skip(3).ToList(), lineNumber);
			if (operands.Count == 0)
			{
				throw new AssemblerException($"Line {lineNumber}: {tokens[0]} requires at least one value.");
			}

			ramDirectives.Add(new DataDirective(kind, label, operands, null, lineNumber));
		}

		/// <summary>Parses an <c>.objstring</c> directive into an <see cref="ObjDataDirective"/> containing a Pascal-layout string.</summary>
		/// <param name="tokens">The tokenized directive line.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		private void ParseObjStringDirective(IReadOnlyList<string> tokens, int lineNumber)
		{
			if (tokens.Count != 4 || tokens[2] != "," || !IsQuotedString(tokens[3]))
			{
				throw new AssemblerException($"Line {lineNumber}: .objstring expects '<label>, \"text\"'.");
			}

			objDataDirectives.Add(new ObjDataDirective(tokens[1], UnescapeString(tokens[3]), null, lineNumber));
		}

		/// <summary>Parses an <c>.objpacked</c> directive into an <see cref="ObjDataDirective"/> containing raw bytes.</summary>
		/// <param name="tokens">The tokenized directive line.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		private void ParseObjPackedDirective(IReadOnlyList<string> tokens, int lineNumber)
		{
			if (tokens.Count < 4 || tokens[2] != ",")
			{
				throw new AssemblerException($"Line {lineNumber}: .objpacked expects '<label>, <byte>, ...'.");
			}

			List<string> operands = ParseCommaSeparated(tokens.Skip(3).ToList(), lineNumber);
			if (operands.Count == 0)
			{
				throw new AssemblerException($"Line {lineNumber}: .objpacked requires at least one byte value.");
			}

			objDataDirectives.Add(new ObjDataDirective(tokens[1], null, operands, lineNumber));
		}

		/// <summary>Throws an <see cref="AssemblerException"/> if a procedure is currently open, since some directives are only valid at module or file scope.</summary>
		/// <param name="directiveName">The directive name to include in the error message.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		private void RequireProcedureClosed(string directiveName, int lineNumber)
		{
			if (currentProcedure is not null)
			{
				throw new AssemblerException($"Line {lineNumber}: directive '{directiveName}' is not valid inside procedure '{currentProcedure.Name}'.");
			}
		}

		/// <summary>
		/// Splits a source line into tokens, respecting quoted strings, character literals, comma separation, and <c>;</c> comments.
		/// </summary>
		/// <param name="line">The raw source line to tokenize.</param>
		/// <returns>A list of tokens, not including the comment portion of the line.</returns>
		private static List<string> Tokenize(string line)
		{
			List<string> tokens = new();
			StringBuilder builder = new();
			bool inString = false;
			bool inChar = false;
			bool escape = false;

			void Flush()
			{
				if (builder.Length > 0)
				{
					tokens.Add(builder.ToString());
					builder.Clear();
				}
			}

			foreach (char character in line)
			{
				if (escape)
				{
					builder.Append(character);
					escape = false;
					continue;
				}

				if ((inString || inChar) && character == '\\')
				{
					builder.Append(character);
					escape = true;
					continue;
				}

				if (inString)
				{
					builder.Append(character);
					if (character == '"')
					{
						tokens.Add(builder.ToString());
						builder.Clear();
						inString = false;
					}

					continue;
				}

				if (inChar)
				{
					builder.Append(character);
					if (character == '\'')
					{
						tokens.Add(builder.ToString());
						builder.Clear();
						inChar = false;
					}

					continue;
				}

				if (character == ';')
				{
					break;
				}

				if (char.IsWhiteSpace(character))
				{
					Flush();
					continue;
				}

				if (character == '"')
				{
					Flush();
					builder.Append(character);
					inString = true;
					continue;
				}

				if (character == '\'')
				{
					Flush();
					builder.Append(character);
					inChar = true;
					continue;
				}

				if (character == ',')
				{
					Flush();
					tokens.Add(character.ToString());
					continue;
				}

				builder.Append(character);
			}

			Flush();
			return tokens;
		}

		/// <summary>Collects the values from an already-tokenized comma-separated list, where commas appear as their own tokens.</summary>
		/// <param name="tokens">The token list to parse, not including any leading keyword.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <returns>The list of value tokens, with commas removed.</returns>
		private static List<string> ParseCommaSeparated(IReadOnlyList<string> tokens, int lineNumber)
		{
			List<string> values = new();
			int index = 0;
			while (index < tokens.Count)
			{
				if (tokens[index] == ",")
				{
					index++;
					continue;
				}

				values.Add(tokens[index]);
				index++;
				if (index < tokens.Count && tokens[index] == ",")
				{
					index++;
				}
				else if (index < tokens.Count)
				{
					throw new AssemblerException($"Line {lineNumber}: expected a comma between operands.");
				}
			}

			return values;
		}

		/// <summary>
		/// Parses a comma-separated list of <c>Name</c> or <c>Name=Value</c> declarations,
		/// such as those used by <c>.global</c> and <c>.local</c>.
		/// </summary>
		/// <param name="tokens">The token list to parse, not including any leading keyword.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <param name="requireInitializer">If <see langword="true"/>, each declaration must include a value.</param>
		/// <returns>A list of parsed <see cref="NamedSlotDeclaration"/> values.</returns>
		private static List<NamedSlotDeclaration> ParseNamedSlotDeclarations(IReadOnlyList<string> tokens, int lineNumber, bool requireInitializer)
		{
			List<string> declarations = ParseCommaSeparatedSegments(tokens, lineNumber);
			if (declarations.Count == 0)
			{
				throw new AssemblerException($"Line {lineNumber}: expected at least one declaration.");
			}

			List<NamedSlotDeclaration> parsed = new();
			foreach (string declaration in declarations)
			{
				int equalsIndex = declaration.IndexOf('=');
				if (equalsIndex < 0)
				{
					if (requireInitializer)
					{
						throw new AssemblerException($"Line {lineNumber}: declaration '{declaration}' must use '<name>=<value>'.");
					}

					parsed.Add(new NamedSlotDeclaration(declaration, null));
			}
				else
				{
					string name = declaration[..equalsIndex].Trim();
					string valueToken = declaration[(equalsIndex + 1)..].Trim();
					if (name.Length == 0 || valueToken.Length == 0)
					{
						throw new AssemblerException($"Line {lineNumber}: declaration '{declaration}' must use '<name>=<value>'.");
					}

					parsed.Add(new NamedSlotDeclaration(name, valueToken));
				}
			}

			return parsed;
		}

		/// <summary>
		/// Splits a comma-separated token list into segments, where each segment may span multiple adjacent tokens
		/// (e.g. tokens that together form a <c>Name=Value</c> expression before the comma).
		/// </summary>
		/// <param name="tokens">The token list to split.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <returns>A list of raw segment strings, one per comma-separated item.</returns>
		private static List<string> ParseCommaSeparatedSegments(IReadOnlyList<string> tokens, int lineNumber)
		{
			List<string> values = new();
			StringBuilder current = new();

			void FlushCurrent()
			{
				if (current.Length == 0)
				{
					throw new AssemblerException($"Line {lineNumber}: expected a declaration between commas.");
				}

				values.Add(current.ToString());
				current.Clear();
			}

			for (int index = 0; index < tokens.Count; index++)
			{
				string token = tokens[index];
				if (token == ",")
				{
					FlushCurrent();
					continue;
				}

				current.Append(token);
			}

			if (current.Length > 0)
			{
				values.Add(current.ToString());
			}
			else if (tokens.Count > 0)
			{
				throw new AssemblerException($"Line {lineNumber}: trailing comma is not allowed.");
			}

			return values;
		}

		/// <summary>Appends one named slot to the parallel value and index-map lists, raising an error on duplicates.</summary>
		/// <param name="values">The list of raw initializer-value tokens for the slots.</param>
		/// <param name="names">The dictionary mapping slot names to their 0-based indices.</param>
		/// <param name="declaration">The parsed <c>Name=Value</c> declaration to register.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <param name="slotKind">A human-readable description of the slot kind (e.g. "program global"), used in error messages.</param>
		private static void AddNamedSlot(List<string> values, Dictionary<string, int> names, NamedSlotDeclaration declaration, int lineNumber, string slotKind)
		{
			if (names.ContainsKey(declaration.Name))
			{
				throw new AssemblerException($"Line {lineNumber}: duplicate {slotKind} name '{declaration.Name}'.");
			}

			string valueToken = declaration.ValueToken
				?? throw new AssemblerException($"Line {lineNumber}: declaration '{declaration.Name}' must use '<name>=<value>'.");
			names[declaration.Name] = values.Count;
			values.Add(valueToken);
		}

		/// <summary>Registers a constant definition in the constant table, raising an error on duplicates.</summary>
		/// <param name="constant">The constant to register.</param>
		private void RegisterConstant(ConstantDefinition constant)
		{
			if (constants.ContainsKey(constant.Name))
			{
				throw new AssemblerException($"Line {constant.LineNumber}: duplicate constant '{constant.Name}'.");
			}

			constants.Add(constant.Name, constant);
		}

		/// <summary>
		/// Tries to parse a token list as a <c>Name = Value</c> constant definition.
		/// Returns <see langword="false"/> if the token list does not match this pattern.
		/// </summary>
		/// <param name="tokens">The token list to examine.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <param name="constant">When this method returns <see langword="true"/>, the parsed constant definition.</param>
		/// <returns><see langword="true"/> if the tokens form a valid constant definition; otherwise <see langword="false"/>.</returns>
		private static bool TryParseConstantDefinition(IReadOnlyList<string> tokens, int lineNumber, out ConstantDefinition? constant)
		{
			constant = null;
			if (tokens.Count == 0)
			{
				return false;
			}

			string joined = string.Concat(tokens);
			int equalsIndex = joined.IndexOf('=');
			if (equalsIndex <= 0 || equalsIndex != joined.LastIndexOf('='))
			{
				return false;
			}

			string name = joined[..equalsIndex];
			string valueToken = joined[(equalsIndex + 1)..];
			if (string.IsNullOrWhiteSpace(name) || string.IsNullOrWhiteSpace(valueToken))
			{
				throw new AssemblerException($"Line {lineNumber}: constant definitions must use 'Name = Value'.");
			}

			constant = new ConstantDefinition(name, valueToken, lineNumber);
			return true;
		}
	}
}
