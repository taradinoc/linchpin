/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Chisel;

internal static partial class Program
{
    /// <summary>
	/// Maintains all symbol tables needed during instruction encoding: data symbols, procedure symbols,
	/// code labels, named constants, and global/local variable indices.
	/// </summary>
	private sealed class SymbolCatalog
	{
		private readonly Dictionary<string, DataSymbol> dataSymbols;
		private readonly Dictionary<string, ProcedureSymbol> procedures;
		private readonly Dictionary<string, CodeLabelSymbol> labels;
		private readonly Dictionary<string, ConstantDefinition> constants;
		private readonly Dictionary<string, int> programGlobals;
		private readonly Dictionary<string, Dictionary<string, int>> moduleGlobals;
		private readonly Dictionary<string, Dictionary<string, int>> locals;

		public SymbolCatalog(
			Dictionary<string, DataSymbol> dataSymbols,
			Dictionary<string, ProcedureSymbol> procedures,
			Dictionary<string, CodeLabelSymbol> labels,
			Dictionary<string, ConstantDefinition> constants,
			Dictionary<string, int> programGlobals,
			Dictionary<string, Dictionary<string, int>> moduleGlobals,
			Dictionary<string, Dictionary<string, int>> locals)
		{
			this.dataSymbols = dataSymbols;
			this.procedures = procedures;
			this.labels = labels;
			this.constants = constants;
			this.programGlobals = programGlobals;
			this.moduleGlobals = moduleGlobals;
			this.locals = locals;
		}

		/// <summary>The data symbols (RAM and object-data) keyed by label name.</summary>
		public IReadOnlyDictionary<string, DataSymbol> DataSymbols => dataSymbols;

		/// <summary>
		/// Resolves a procedure reference token to its <see cref="ProcedureSymbol"/>.
		/// Looks up by fully qualified name, by local name within the current module, or by unambiguous bare name.
		/// </summary>
		/// <param name="token">The procedure reference token (e.g. <c>MyProc</c> or <c>MyModule.MyProc</c>).</param>
		/// <param name="currentModule">The name of the module containing the reference, or <see langword="null"/> for top-level context.</param>
		/// <param name="currentProcedure">The name of the procedure containing the reference, or <see langword="null"/> for top-level context.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <returns>The resolved <see cref="ProcedureSymbol"/>.</returns>
		public ProcedureSymbol ResolveProcedureReference(string token, string? currentModule, string? currentProcedure, int lineNumber)
		{
			if (procedures.TryGetValue(token, out ProcedureSymbol? exact))
			{
				return exact;
			}

			if (currentModule is not null && procedures.TryGetValue(QualifyProcedure(currentModule, token), out ProcedureSymbol? local))
			{
				return local;
			}

			List<ProcedureSymbol> matches = procedures.Values.Where(symbol => symbol.ProcedureName.Equals(token, StringComparison.OrdinalIgnoreCase)).ToList();
			if (matches.Count == 1)
			{
				return matches[0];
			}

			if (matches.Count > 1)
			{
				throw new AssemblerException($"Line {lineNumber}: procedure reference '{token}' is ambiguous; qualify it as Module.Procedure.");
			}

			throw new AssemblerException($"Line {lineNumber}: unknown procedure '{token}'.");
		}

		/// <summary>
		/// Resolves a code target token to a module-relative byte offset.
		/// Accepts local labels, fully qualified labels, and procedure names.
		/// </summary>
		/// <param name="token">The target token to resolve.</param>
		/// <param name="moduleName">The name of the module containing the branch instruction.</param>
		/// <param name="procedureName">The name of the procedure containing the branch instruction.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <param name="requireSameModule">If <see langword="true"/>, raises an error if the target is in a different module.</param>
		/// <returns>The module-relative byte offset of the target.</returns>
		public int ResolveCodeOffset(string token, string moduleName, string procedureName, int lineNumber, bool requireSameModule)
		{
			if (TryParseInteger(token, out int numeric))
			{
				return numeric;
			}

			if (labels.TryGetValue(QualifyLabel(moduleName, procedureName, token), out CodeLabelSymbol? localLabel))
			{
				return localLabel.ModuleOffset;
			}

			if (labels.TryGetValue(token, out CodeLabelSymbol? qualifiedLabel))
			{
				if (requireSameModule && !qualifiedLabel.ModuleName.Equals(moduleName, StringComparison.OrdinalIgnoreCase))
				{
					throw new AssemblerException($"Line {lineNumber}: code target '{token}' must stay within module '{moduleName}'.");
				}

				return qualifiedLabel.ModuleOffset;
			}

			ProcedureSymbol procedure = ResolveProcedureReference(token, moduleName, procedureName, lineNumber);
			if (requireSameModule && procedure.ModuleId != ResolveProcedureReference(QualifyProcedure(moduleName, procedureName), null, null, lineNumber).ModuleId)
			{
				throw new AssemblerException($"Line {lineNumber}: code target '{token}' must stay within module '{moduleName}'.");
			}

			return procedure.ModuleOffset;
		}

		/// <summary>Resolves a labeled RAM symbol to its word address.</summary>
		/// <param name="token">The symbol name to resolve.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <returns>The word address of the symbol.</returns>
		public ushort ResolveDataAddress(string token, int lineNumber)
		{
			if (dataSymbols.TryGetValue(token, out DataSymbol? symbol))
			{
				return symbol.WordAddress;
			}

			throw new AssemblerException($"Line {lineNumber}: unknown RAM symbol '{token}'.");
		}

		/// <summary>Resolves a labeled RAM symbol to its byte address (word address multiplied by 2).</summary>
		/// <param name="token">The symbol name to resolve.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <returns>The byte address of the symbol.</returns>
		public ushort ResolveDataByteAddress(string token, int lineNumber)
		{
			ushort wordAddress = ResolveDataAddress(token, lineNumber);
			return checked((ushort)(wordAddress * 2));
		}

		/// <summary>
		/// Evaluates a word-sized operand token, resolving constants, data symbols, and arithmetic expressions.
		/// </summary>
		/// <param name="token">The operand token or expression to evaluate.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <returns>The resolved 16-bit value.</returns>
		public ushort ResolveWordOperand(string token, int lineNumber)
		{
			int numeric = ResolveNumericExpression(token, lineNumber, new HashSet<string>(StringComparer.OrdinalIgnoreCase));
			if (numeric < short.MinValue || numeric > ushort.MaxValue)
			{
				throw new AssemblerException($"Line {lineNumber}: value '{token}' does not fit in a word.");
			}

			return unchecked((ushort)numeric);
		}

		/// <summary>Evaluates a named constant to its numeric value, throwing if the name is not found.</summary>
		/// <param name="token">The constant name to resolve.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <returns>The numeric value of the constant.</returns>
		public int ResolveConstantNumeric(string token, int lineNumber)
		{
			if (!TryResolveConstantNumeric(token, lineNumber, out int value))
			{
				throw new AssemblerException($"Line {lineNumber}: unknown constant '{token}'.");
			}

			return value;
		}

		/// <summary>
		/// Tries to evaluate a token as a named constant, returning <see langword="false"/> if the name is not known.
		/// </summary>
		/// <param name="token">The token to look up as a constant.</param>
		/// <param name="lineNumber">The source line number, used in error messages during evaluation.</param>
		/// <param name="value">When this method returns <see langword="true"/>, contains the constant's numeric value.</param>
		/// <returns><see langword="true"/> if the token names a known constant; otherwise <see langword="false"/>.</returns>
		public bool TryResolveConstantNumeric(string token, int lineNumber, out int value)
		{
			if (!constants.ContainsKey(token))
			{
				value = 0;
				return false;
			}

			value = ResolveNumericExpression(token, lineNumber, new HashSet<string>(StringComparer.OrdinalIgnoreCase));
			return true;
		}

		/// <summary>
		/// Recursively evaluates a token that may be a numeric literal, a named constant, a data symbol,
		/// or a procedure selector expression. Detects and reports circular constant definitions.
		/// </summary>
		/// <param name="token">The token or expression to evaluate.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <param name="resolving">The set of constant names currently being resolved, used for cycle detection.</param>
		/// <returns>The numeric value of the expression.</returns>
		private int ResolveNumericExpression(string token, int lineNumber, HashSet<string> resolving)
		{
			if (TryParseInteger(token, out int numeric))
			{
				return numeric;
			}

			if (constants.TryGetValue(token, out ConstantDefinition? constant))
			{
				if (!resolving.Add(constant.Name))
				{
					throw new AssemblerException($"Line {lineNumber}: constant '{constant.Name}' is recursive.");
				}

				try
				{
					return ResolveNumericExpression(constant.ValueToken, constant.LineNumber, resolving);
				}
				finally
				{
					resolving.Remove(constant.Name);
				}
			}

			if (dataSymbols.TryGetValue(token, out DataSymbol? dataSymbol))
			{
				return dataSymbol.WordAddress;
			}

			ProcedureSymbol procedure = ResolveProcedureReference(token, null, null, lineNumber);
			if (procedure.ProcedureIndex < 0)
			{
				throw new AssemblerException($"Line {lineNumber}: procedure reference '{token}' is private; add an .export directive or use a numeric selector.");
			}

			return checked((procedure.ModuleId << 8) | procedure.ProcedureIndex);
		}

		/// <summary>
		/// Resolves a local variable token to its 0-based index, accepting a numeric literal, a constant name,
		/// or a declared local name.
		/// </summary>
		/// <param name="token">The local variable token to resolve.</param>
		/// <param name="moduleName">The name of the enclosing module.</param>
		/// <param name="procedureName">The name of the enclosing procedure.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <returns>The 0-based local variable index as a byte.</returns>
		public byte ResolveLocalIndex(string token, string moduleName, string procedureName, int lineNumber)
		{
			if (TryParseInteger(token, out int numeric))
			{
				if (numeric < 0 || numeric > 0xFF)
				{
					throw new AssemblerException($"Line {lineNumber}: value '{token}' does not fit in a byte.");
				}

				return (byte)numeric;
			}

			if (constants.ContainsKey(token))
			{
				int constantValue = ResolveConstantNumeric(token, lineNumber);
				if (constantValue < 0 || constantValue > 0xFF)
				{
					throw new AssemblerException($"Line {lineNumber}: value '{token}' does not fit in a byte.");
				}

				return (byte)constantValue;
			}

			if (!locals.TryGetValue(QualifyProcedure(moduleName, procedureName), out Dictionary<string, int>? localNames)
				|| !localNames.TryGetValue(token, out int localIndex))
			{
				throw new AssemblerException($"Line {lineNumber}: unknown local '{token}' in procedure '{moduleName}.{procedureName}'.");
			}

			return checked((byte)localIndex);
		}

		/// <summary>
		/// Resolves a program-global token to its 0-based index, accepting a numeric literal, a constant name,
		/// or a declared program-global name.
		/// </summary>
		/// <param name="token">The program-global token to resolve.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <returns>The 0-based program-global index as a byte.</returns>
		public byte ResolveProgramGlobalIndex(string token, int lineNumber)
		{
			if (TryParseInteger(token, out int numeric))
			{
				if (numeric < 0 || numeric > 0xFF)
				{
					throw new AssemblerException($"Line {lineNumber}: value '{token}' does not fit in a byte.");
				}

				return (byte)numeric;
			}

			if (constants.ContainsKey(token))
			{
				int constantValue = ResolveConstantNumeric(token, lineNumber);
				if (constantValue < 0 || constantValue > 0xFF)
				{
					throw new AssemblerException($"Line {lineNumber}: value '{token}' does not fit in a byte.");
				}

				return (byte)constantValue;
			}

			if (!programGlobals.TryGetValue(token, out int globalIndex))
			{
				throw new AssemblerException($"Line {lineNumber}: unknown program global '{token}'.");
			}

			return checked((byte)globalIndex);
		}

		/// <summary>
		/// Resolves a module-global token to its 0-based index within the specified module,
		/// accepting a numeric literal, a constant name, or a declared module-global name.
		/// </summary>
		/// <param name="token">The module-global token to resolve.</param>
		/// <param name="moduleName">The module whose global table to search.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <returns>The 0-based module-global index as a byte.</returns>
		public byte ResolveModuleGlobalIndex(string token, string moduleName, int lineNumber)
		{
			if (TryParseInteger(token, out int numeric))
			{
				if (numeric < 0 || numeric > 0xFF)
				{
					throw new AssemblerException($"Line {lineNumber}: value '{token}' does not fit in a byte.");
				}

				return (byte)numeric;
			}

			if (constants.ContainsKey(token))
			{
				int constantValue = ResolveConstantNumeric(token, lineNumber);
				if (constantValue < 0 || constantValue > 0xFF)
				{
					throw new AssemblerException($"Line {lineNumber}: value '{token}' does not fit in a byte.");
				}

				return (byte)constantValue;
			}

			if (!moduleGlobals.TryGetValue(moduleName, out Dictionary<string, int>? names)
				|| !names.TryGetValue(token, out int globalIndex))
			{
				throw new AssemblerException($"Line {lineNumber}: unknown module global '{token}' in module '{moduleName}'.");
			}

			return checked((byte)globalIndex);
		}
	}
}
