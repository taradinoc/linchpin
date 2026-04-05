/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.IO.Compression;
using System.Text;

namespace Chisel;

internal static partial class Program
{
    /// <summary>
	/// Performs multi-pass assembly of a parsed <see cref="AssemblySource"/> into a binary <see cref="AssemblyImage"/>.
	/// Assembly requires multiple layout passes to converge on stable instruction sizes, because branch instruction
	/// encoding depends on the distance to target labels, which in turn depends on the sizes of intervening instructions.
	/// </summary>
	private sealed class Assembler
	{
		private readonly GrammarCatalog grammar;
		private readonly AssemblySource source;

		public Assembler(GrammarCatalog grammar, AssemblySource source)
		{
			this.grammar = grammar;
			this.source = source;
		}

		/// <summary>Assembles the parsed source into a complete <see cref="AssemblyImage"/> containing OBJ and MME binary data.</summary>
		/// <returns>The assembled image with object bytes, metadata bytes, and summary information.</returns>
		public AssemblyImage Assemble()
		{
			if (source.Modules.Count == 0)
			{
				throw new AssemblerException("Source does not declare any modules.");
			}

			AssignModuleAndProcedureIds();
			ObjDataLayout objDataLayout = LayoutObjData();
			RamLayout ramLayout = LayoutRam(objDataLayout.Symbols);
			Dictionary<string, DataSymbol> dataSymbols = CombineDataSymbols(ramLayout.Symbols, objDataLayout.Symbols);
			IReadOnlyList<ModuleLayout> moduleLayouts = LayoutModules(dataSymbols);
			SymbolCatalog symbols = BuildFinalSymbols(dataSymbols, moduleLayouts);
			ObjectImageLayout objectImage = BuildObjectImage(moduleLayouts, objDataLayout.Bytes);
			byte[] metadataBytes = BuildMetadataImage(ramLayout, moduleLayouts, objectImage.CodeByteLength, symbols);
			ProcedureSymbol entryProcedure = symbols.ResolveProcedureReference(source.EntrySymbol, null, null, source.EntryLine);

			return new AssemblyImage(
				objectImage.Bytes,
				metadataBytes,
				moduleLayouts,
				ramLayout.InitialRamWords,
				entryProcedure.ModuleId,
				entryProcedure.ProcedureIndex);
		}

		/// <summary>Merges two data-symbol dictionaries into one, raising an error if any label appears in both.</summary>
		/// <param name="first">The first symbol dictionary.</param>
		/// <param name="second">The second symbol dictionary.</param>
		/// <returns>A new dictionary containing all entries from both inputs.</returns>
		private static Dictionary<string, DataSymbol> CombineDataSymbols(
			IReadOnlyDictionary<string, DataSymbol> first,
			IReadOnlyDictionary<string, DataSymbol> second)
		{
			Dictionary<string, DataSymbol> combined = new(first, StringComparer.OrdinalIgnoreCase);
			foreach ((string key, DataSymbol value) in second)
			{
				if (!combined.TryAdd(key, value))
				{
					throw new AssemblerException($"Duplicate data symbol '{key}'.");
				}
			}

			return combined;
		}

		/// <summary>
		/// Assigns 1-based module IDs to all modules and resolves each module's export table, mapping exported
		/// procedure names to their 0-based procedure-table indices.
		/// </summary>
		private void AssignModuleAndProcedureIds()
		{
			int moduleId = 1;
			foreach (ModuleDefinition module in source.Modules)
			{
				module.ModuleId = moduleId++;

				foreach (ProcedureDefinition procedure in module.Procedures)
				{
					procedure.ProcedureIndex = -1;
				}

				Dictionary<string, ProcedureDefinition> proceduresByName = module.Procedures.ToDictionary(procedure => procedure.Name, StringComparer.OrdinalIgnoreCase);
				HashSet<int> assignedIndices = new();
				foreach (ProcedureExportDefinition export in module.Exports)
				{
					if (!proceduresByName.TryGetValue(export.ProcedureName, out ProcedureDefinition? procedure))
					{
						throw new AssemblerException($"Line {export.LineNumber}: module '{module.Name}' does not define exported procedure '{export.ProcedureName}'.");
					}

					if (procedure.ProcedureIndex >= 0)
					{
						throw new AssemblerException($"Line {export.LineNumber}: procedure '{export.ProcedureName}' is exported more than once in module '{module.Name}'.");
					}

					if (!assignedIndices.Add(export.ExportedProcedureIndex))
					{
						throw new AssemblerException($"Line {export.LineNumber}: module '{module.Name}' assigns export index {export.ExportedProcedureIndex} more than once.");
					}

					procedure.ProcedureIndex = export.ExportedProcedureIndex;
				}

				for (int expectedIndex = 0; expectedIndex < module.Exports.Count; expectedIndex++)
				{
					if (!assignedIndices.Contains(expectedIndex))
					{
						throw new AssemblerException($"Module '{module.Name}' export indices must be contiguous from 0 through {module.Exports.Count - 1}; missing {expectedIndex}.");
					}
				}
			}
		}

		/// <summary>
		/// Lays out all RAM data directives: allocates word addresses, evaluates initial values,
		/// and builds the initial RAM byte image.
		/// </summary>
		/// <param name="externalDataSymbols">Data symbols from the object-data section, available for cross-referencing during RAM encoding.</param>
		/// <returns>A <see cref="RamLayout"/> containing the encoded RAM bytes, word count, and symbol table.</returns>
		private RamLayout LayoutRam(IReadOnlyDictionary<string, DataSymbol> externalDataSymbols)
		{
			ushort currentWordAddress = 0;
			List<AllocatedRamItem> allocatedItems = new();
			Dictionary<string, DataSymbol> dataSymbols = new(StringComparer.OrdinalIgnoreCase);
			PendingRamBlock? currentBlock = null;

			void FinalizeCurrentBlock()
			{
				if (currentBlock is null)
				{
					return;
				}

				int byteLength = AlignEven(currentBlock.Directives.Sum(GetDataByteLength));
				allocatedItems.Add(new AllocatedRamItem(currentBlock.Label, currentBlock.WordAddress, currentBlock.Directives.ToArray(), byteLength));
				currentWordAddress = checked((ushort)(currentBlock.WordAddress + (byteLength / 2)));
				currentBlock = null;
			}

			foreach (RamDirective directive in source.RamDirectives)
			{
				switch (directive)
				{
					case RamOriginDirective originDirective:
					{
						FinalizeCurrentBlock();
						ushort origin = checked((ushort)EvaluateNumericLiteral(originDirective.ValueToken, originDirective.LineNumber, 0, ushort.MaxValue));
						currentWordAddress = origin;
						break;
					}
					case DataDirective dataDirective:
					{
						if (currentBlock is null || !currentBlock.Label.Equals(dataDirective.Label, StringComparison.OrdinalIgnoreCase))
						{
							FinalizeCurrentBlock();
							if (dataSymbols.ContainsKey(dataDirective.Label))
							{
								throw new AssemblerException($"Line {dataDirective.LineNumber}: duplicate data label '{dataDirective.Label}'.");
							}

							currentBlock = new PendingRamBlock(dataDirective.Label, dataDirective.LineNumber, currentWordAddress);
							dataSymbols.Add(dataDirective.Label, new DataSymbol(dataDirective.Label, currentWordAddress, dataDirective.LineNumber));
						}

						currentBlock.Directives.Add(dataDirective);
						break;
					}
				}
			}

			FinalizeCurrentBlock();

			int totalBytes = allocatedItems.Count == 0
				? 0
				: allocatedItems.Max(item => item.WordAddress * 2 + item.ByteLength);
			byte[] initialRam = new byte[(totalBytes + 1) & ~1];
			SymbolCatalog ramSymbols = BuildRamSymbols(CombineDataSymbols(dataSymbols, externalDataSymbols));

			foreach (AllocatedRamItem item in allocatedItems)
			{
				byte[] encoded = EncodeDataBlock(item.Directives, ramSymbols);
				if (encoded.Length != item.ByteLength)
				{
					throw new AssemblerException($"Internal error while encoding RAM item '{item.Label}'.");
				}

				Buffer.BlockCopy(encoded, 0, initialRam, item.WordAddress * 2, encoded.Length);
			}

			return new RamLayout(initialRam, initialRam.Length / 2, dataSymbols);
		}

		/// <summary>
		/// Lays out all object-data directives (<c>.objstring</c>, <c>.objpacked</c>, and <c>.objinclude</c>) into the read-only
		/// data section that is appended to the OBJ file after the code.
		/// </summary>
		/// <returns>An <see cref="ObjDataLayout"/> containing the encoded bytes and symbol table.</returns>
		private ObjDataLayout LayoutObjData()
		{
			Dictionary<string, DataSymbol> dataSymbols = new(StringComparer.OrdinalIgnoreCase);
			List<AllocatedObjDataItem> items = new();
			int currentByteOffset = 2;

			foreach (ObjDataDirective directive in source.ObjDataDirectives)
			{
				if (dataSymbols.ContainsKey(directive.Label))
				{
					throw new AssemblerException($"Line {directive.LineNumber}: duplicate data label '{directive.Label}'.");
				}

				if ((currentByteOffset & 1) != 0)
				{
					currentByteOffset++;
				}

				ushort wordAddress = checked((ushort)(currentByteOffset / 2));
				byte[] encoded = EncodeObjDataDirective(directive);
				dataSymbols.Add(directive.Label, new DataSymbol(directive.Label, wordAddress, directive.LineNumber));
				items.Add(new AllocatedObjDataItem(directive.Label, currentByteOffset, encoded, directive.LineNumber));
				currentByteOffset += encoded.Length;
			}

			byte[] bytes = new byte[(currentByteOffset + 1) & ~1];
			foreach (AllocatedObjDataItem item in items)
			{
				Buffer.BlockCopy(item.Bytes, 0, bytes, item.ByteOffset, item.Bytes.Length);
			}

			return new ObjDataLayout(bytes, dataSymbols);
		}

		/// <summary>Encodes one object-data directive into its byte payload.</summary>
		/// <param name="directive">The parsed object-data directive.</param>
		/// <returns>The encoded byte payload.</returns>
		private static byte[] EncodeObjDataDirective(ObjDataDirective directive)
		{
			if (directive.PackedValues is not null)
			{
				return directive.PackedValues.Select(value => ParseByteToken(value, directive.LineNumber)).ToArray();
			}

			if (directive.StringValue is not null)
			{
				return EncodeObjString(directive.StringValue, directive.LineNumber);
			}

			if (directive.IncludedFilePath is not null)
			{
				return LoadIncludedFileBytes(directive);
			}

			throw new AssemblerException($"Line {directive.LineNumber}: object-data directive '{directive.Label}' has no payload.");
		}

		private static byte[] LoadIncludedFileBytes(ObjDataDirective directive)
		{
			string includedFilePath = directive.IncludedFilePath ?? throw new InvalidOperationException("Included file path is required.");
			if (!File.Exists(includedFilePath))
			{
				throw new AssemblerException($"Line {directive.LineNumber}: objinclude file '{includedFilePath}' does not exist.");
			}

			try
			{
				byte[] bytes = HasGzipExtension(includedFilePath)
					? ReadGzipFile(includedFilePath)
					: File.ReadAllBytes(includedFilePath);
				if (bytes.Length == 0)
				{
					throw new AssemblerException($"Line {directive.LineNumber}: objinclude file '{includedFilePath}' is empty.");
				}

				return bytes;
			}
			catch (InvalidDataException ex) when (HasGzipExtension(includedFilePath))
			{
				throw new AssemblerException($"Line {directive.LineNumber}: objinclude gzip file '{includedFilePath}' could not be decompressed: {ex.Message}");
			}
		}

		private static bool HasGzipExtension(string filePath) =>
			string.Equals(Path.GetExtension(filePath), ".gz", StringComparison.OrdinalIgnoreCase);

		private static byte[] ReadGzipFile(string filePath)
		{
			using FileStream input = File.OpenRead(filePath);
			using GZipStream gzip = new(input, CompressionMode.Decompress);
			using MemoryStream output = new();
			gzip.CopyTo(output);
			return output.ToArray();
		}

		/// <summary>
		/// Encodes a string value as a Pascal-layout VM string: a 16-bit length word followed by the raw ASCII bytes.
		/// </summary>
		/// <param name="value">The string to encode.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <returns>The encoded bytes.</returns>
		private static byte[] EncodeObjString(string value, int lineNumber)
		{
			byte[] text = GetAsciiBytes(value, lineNumber);
			List<byte> encodedBytes = new(text.Length + 2);
			WriteWord(encodedBytes, checked((ushort)text.Length));
			encodedBytes.AddRange(text);
			return encodedBytes.ToArray();
		}

		/// <summary>Parses a single byte token for use in an <c>.objpacked</c> directive.</summary>
		/// <param name="token">The token representing a byte value.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <returns>The parsed byte value.</returns>
		private static byte ParseByteToken(string token, int lineNumber)
		{
			if (!TryParseInteger(token, out int numeric) || numeric is < 0 or > 0xFF)
			{
				throw new AssemblerException($"Line {lineNumber}: objpacked byte '{token}' is not a valid byte value.");
			}

			return checked((byte)numeric);
		}

		/// <summary>
		/// Constructs a <see cref="SymbolCatalog"/> suitable for evaluating RAM initial values.
		/// Procedure offsets are not yet final at this stage, so procedures are registered with offset 0.
		/// </summary>
		/// <param name="dataSymbols">The combined RAM and object-data symbols.</param>
		/// <returns>A <see cref="SymbolCatalog"/> for use during RAM encoding.</returns>
		private SymbolCatalog BuildRamSymbols(IReadOnlyDictionary<string, DataSymbol> dataSymbols)
		{
			Dictionary<string, DataSymbol> dataByName = new(dataSymbols, StringComparer.OrdinalIgnoreCase);
			Dictionary<string, ProcedureSymbol> procedures = new(StringComparer.OrdinalIgnoreCase);
			Dictionary<string, CodeLabelSymbol> labels = new(StringComparer.OrdinalIgnoreCase);
			Dictionary<string, ConstantDefinition> constants = new(source.Constants, StringComparer.OrdinalIgnoreCase);
			Dictionary<string, int> programGlobals = new(source.ProgramGlobalNames, StringComparer.OrdinalIgnoreCase);
			Dictionary<string, Dictionary<string, int>> moduleGlobals = new(StringComparer.OrdinalIgnoreCase);
			Dictionary<string, Dictionary<string, int>> locals = new(StringComparer.OrdinalIgnoreCase);

			foreach (ModuleDefinition module in source.Modules)
			{
				moduleGlobals[module.Name] = new Dictionary<string, int>(module.ModuleGlobalNames, StringComparer.OrdinalIgnoreCase);

				foreach (ProcedureDefinition procedure in module.Procedures)
				{
					locals[QualifyProcedure(module.Name, procedure.Name)] = new Dictionary<string, int>(procedure.LocalNames, StringComparer.OrdinalIgnoreCase);
					procedures[QualifyProcedure(module.Name, procedure.Name)] = new ProcedureSymbol(
						module.Name,
						procedure.Name,
						module.ModuleId,
						procedure.ProcedureIndex,
						0,
						procedure.LineNumber);
				}
			}

			return new SymbolCatalog(dataByName, procedures, labels, constants, programGlobals, moduleGlobals, locals);
		}

		/// <summary>
		/// Runs the multi-pass layout loop until all symbol offsets and instruction sizes converge,
		/// then encodes all modules to bytes using the final stable layout.
		/// </summary>
		/// <param name="dataSymbols">The combined data symbol table used to resolve operands.</param>
		/// <returns>A list of <see cref="ModuleLayout"/> objects with encoded bytes populated.</returns>
		private IReadOnlyList<ModuleLayout> LayoutModules(IReadOnlyDictionary<string, DataSymbol> dataSymbols)
		{
			SymbolCatalog preLayoutSymbols = BuildRamSymbols(dataSymbols);
			Dictionary<string, int> previousSymbolOffsets = new(StringComparer.OrdinalIgnoreCase);
			Dictionary<InstructionDefinition, int> previousInstructionSizes = new();
			List<ModuleLayout> finalLayouts = new();
			List<byte[]> finalModuleBytes = new();
			string? lastConvergenceDetail = null;

			for (int pass = 0; pass < 24; pass++)
			{
				Dictionary<string, int> currentSymbolOffsets = new(StringComparer.OrdinalIgnoreCase);
				Dictionary<InstructionDefinition, int> currentInstructionSizes = new();
				List<ModuleLayout> currentLayouts = new();

				foreach (ModuleDefinition module in source.Modules)
				{
					ModuleLayout moduleLayout = LayoutModule(
						module,
						preLayoutSymbols,
						previousSymbolOffsets,
						previousInstructionSizes,
						currentSymbolOffsets,
						currentInstructionSizes);
					currentLayouts.Add(moduleLayout);
				}

				SymbolCatalog symbols = BuildFinalSymbols(dataSymbols, currentLayouts);
				Dictionary<InstructionDefinition, int> encodedInstructionSizes = new(currentInstructionSizes);
				List<byte[]> encodedModules = new();
				bool layoutMatchedEncoding = true;
				foreach (ModuleLayout layout in currentLayouts)
				{
					if (!TryEncodeModule(layout, symbols, encodedInstructionSizes, out byte[] moduleBytes, out string? failureDetail))
					{
						layoutMatchedEncoding = false;
						lastConvergenceDetail = failureDetail;
						break;
					}

					encodedModules.Add(moduleBytes);
				}

				if (layoutMatchedEncoding)
				{
					lastConvergenceDetail = DescribeFirstLayoutMismatch(currentSymbolOffsets, previousSymbolOffsets, encodedInstructionSizes, previousInstructionSizes)
						?? "layout stabilized";
				}

				if (layoutMatchedEncoding
					&& LayoutsMatch(previousSymbolOffsets, currentSymbolOffsets, previousInstructionSizes, encodedInstructionSizes))
				{
					finalLayouts = currentLayouts;
					finalModuleBytes = encodedModules;
					break;
				}

				previousSymbolOffsets = currentSymbolOffsets;
				previousInstructionSizes = encodedInstructionSizes;
				finalLayouts = currentLayouts;
				finalModuleBytes = encodedModules;
			}

			if (finalLayouts.Count == 0 || finalModuleBytes.Count != finalLayouts.Count)
			{
				throw new AssemblerException($"Internal error: module layout did not converge. {lastConvergenceDetail}");
			}

			for (int index = 0; index < finalLayouts.Count; index++)
			{
				finalLayouts[index].Bytes = finalModuleBytes[index];
			}

			return finalLayouts;
		}

		/// <summary>
		/// Returns a human-readable description of the first symbol offset or instruction size that differs
		/// between two layout passes, or <see langword="null"/> if both passes agree.
		/// </summary>
		/// <param name="currentSymbolOffsets">Symbol offsets computed in the current pass.</param>
		/// <param name="previousSymbolOffsets">Symbol offsets computed in the previous pass.</param>
		/// <param name="currentInstructionSizes">Instruction sizes computed in the current pass.</param>
		/// <param name="previousInstructionSizes">Instruction sizes computed in the previous pass.</param>
		/// <returns>A mismatch description string, or <see langword="null"/> if the passes are identical.</returns>
		private static string? DescribeFirstLayoutMismatch(
			IReadOnlyDictionary<string, int> currentSymbolOffsets,
			IReadOnlyDictionary<string, int> previousSymbolOffsets,
			IReadOnlyDictionary<InstructionDefinition, int> currentInstructionSizes,
			IReadOnlyDictionary<InstructionDefinition, int> previousInstructionSizes)
		{
			foreach ((string key, int value) in currentSymbolOffsets)
			{
				if (!previousSymbolOffsets.TryGetValue(key, out int previous) || previous != value)
				{
					return $"symbol '{key}' changed from 0x{previous:X4} to 0x{value:X4}.";
				}
			}

			foreach ((InstructionDefinition instruction, int value) in currentInstructionSizes)
			{
				if (!previousInstructionSizes.TryGetValue(instruction, out int previous) || previous != value)
				{
					return $"instruction on line {instruction.LineNumber} changed size from {previous} to {value}.";
				}
			}

			return null;
		}

		/// <summary>
		/// Computes the layout of a single module in one pass: assigns procedure start offsets,
		/// statement offsets, and estimated instruction sizes, recording them in the provided accumulators.
		/// </summary>
		/// <param name="module">The module to lay out.</param>
		/// <param name="symbols">The symbol catalog (with offsets from a previous pass) used for size estimation.</param>
		/// <param name="previousSymbolOffsets">Symbol offsets from the previous pass, used to estimate branch sizes.</param>
		/// <param name="previousInstructionSizes">Instruction sizes from the previous pass, used as fallback estimates.</param>
		/// <param name="currentSymbolOffsets">Accumulator for symbol offsets computed in this pass.</param>
		/// <param name="currentInstructionSizes">Accumulator for instruction sizes computed in this pass.</param>
		/// <returns>A <see cref="ModuleLayout"/> describing all procedure and statement placements.</returns>
		private ModuleLayout LayoutModule(
			ModuleDefinition module,
			SymbolCatalog symbols,
			IReadOnlyDictionary<string, int> previousSymbolOffsets,
			IReadOnlyDictionary<InstructionDefinition, int> previousInstructionSizes,
			IDictionary<string, int> currentSymbolOffsets,
			IDictionary<InstructionDefinition, int> currentInstructionSizes)
		{
			List<ProcedureLayout> procedures = new();
			int currentOffset = 0;

			foreach (ProcedureDefinition procedure in module.Procedures)
			{
				int headerSize = GetProcedureHeaderSize(module, procedure, symbols);
				int procedureStart = AlignProcedureStart(currentOffset, headerSize, procedure.LineNumber);
				currentSymbolOffsets[QualifyProcedure(module.Name, procedure.Name)] = procedureStart;

				int codeOffset = procedureStart + headerSize;
				int statementOffset = codeOffset;
				Dictionary<Statement, int> statementOffsets = new();
				Dictionary<string, int> localLabels = new(StringComparer.OrdinalIgnoreCase);

				foreach (Statement statement in procedure.Statements)
				{
					if (statement is LabelStatement label)
					{
						localLabels[label.Name] = statementOffset;
						currentSymbolOffsets[QualifyLabel(module.Name, procedure.Name, label.Name)] = statementOffset;
						continue;
					}

					InstructionDefinition instruction = (InstructionDefinition)statement;
					int size = EstimateInstructionSize(
						module,
						procedure,
						instruction,
						statementOffset,
						previousSymbolOffsets,
						previousInstructionSizes,
						out bool isProvisional);
					if (ShouldAutoInsertNextBlock(instruction, statementOffset, size))
					{
						statementOffset += RemainingBytesInBlock(statementOffset);
						size = EstimateInstructionSize(
							module,
							procedure,
							instruction,
							statementOffset,
							previousSymbolOffsets,
							previousInstructionSizes,
							out isProvisional);
					}

					statementOffsets[statement] = statementOffset;

					if (!instruction.Mnemonic.Equals("NEXTB", StringComparison.OrdinalIgnoreCase)
						&& size > AvailableBytesBeforeAutoNextBlock(statementOffset)
						&& !isProvisional)
					{
						throw new AssemblerException(
							$"Line {instruction.LineNumber}: instruction '{instruction.Mnemonic}' is too large for a single 256-byte block.");
					}

					currentInstructionSizes[instruction] = size;
					statementOffset += size;
				}

				ProcedureLayout procedureLayout = new(module, procedure, procedureStart, codeOffset, statementOffsets, localLabels, statementOffset - procedureStart);
				procedures.Add(procedureLayout);
				currentOffset = statementOffset;
			}

			return new ModuleLayout(module, procedures);
		}

		/// <summary>Returns <see langword="true"/> if two layout-pass snapshots agree on all symbol offsets and instruction sizes.</summary>
		/// <param name="previousSymbolOffsets">Symbol offsets from the previous pass.</param>
		/// <param name="currentSymbolOffsets">Symbol offsets from the current pass.</param>
		/// <param name="previousInstructionSizes">Instruction sizes from the previous pass.</param>
		/// <param name="currentInstructionSizes">Instruction sizes from the current pass.</param>
		/// <returns><see langword="true"/> if both passes produced identical results.</returns>
		private static bool LayoutsMatch(
			IReadOnlyDictionary<string, int> previousSymbolOffsets,
			IReadOnlyDictionary<string, int> currentSymbolOffsets,
			IReadOnlyDictionary<InstructionDefinition, int> previousInstructionSizes,
			IReadOnlyDictionary<InstructionDefinition, int> currentInstructionSizes)
		{
			if (previousSymbolOffsets.Count != currentSymbolOffsets.Count || previousInstructionSizes.Count != currentInstructionSizes.Count)
			{
				return false;
			}

			foreach ((string key, int value) in currentSymbolOffsets)
			{
				if (!previousSymbolOffsets.TryGetValue(key, out int previous) || previous != value)
				{
					return false;
				}
			}

			foreach ((InstructionDefinition instruction, int value) in currentInstructionSizes)
			{
				if (!previousInstructionSizes.TryGetValue(instruction, out int previous) || previous != value)
				{
					return false;
				}
			}

			return true;
		}

		/// <summary>
		/// Estimates the byte size of an instruction based on its opcode and operands.
		/// For branch instructions with mixed-size jump targets, uses the previous pass's size as a fallback
		/// if the target offset is not yet known.
		/// </summary>
		/// <param name="module">The module containing the instruction.</param>
		/// <param name="procedure">The procedure containing the instruction.</param>
		/// <param name="instruction">The instruction to size.</param>
		/// <param name="statementOffset">The module-relative byte offset at which this instruction will be placed.</param>
		/// <param name="previousSymbolOffsets">Symbol offsets from the previous pass, used to resolve branch targets.</param>
		/// <param name="previousInstructionSizes">Instruction sizes from the previous pass, used as fallbacks.</param>
		/// <param name="isProvisional">Set to <see langword="true"/> if the returned size is a best-effort estimate that may change.</param>
		/// <returns>The estimated byte size of the instruction.</returns>
		private int EstimateInstructionSize(
			ModuleDefinition module,
			ProcedureDefinition procedure,
			InstructionDefinition instruction,
			int statementOffset,
			IReadOnlyDictionary<string, int> previousSymbolOffsets,
			IReadOnlyDictionary<InstructionDefinition, int> previousInstructionSizes,
			out bool isProvisional)
		{
			isProvisional = false;
			ResolvedInstruction resolved = ResolveInstructionEncoding(module, procedure, instruction);
			OpcodeInfo opcode = resolved.Opcode;
			if (opcode.IsNextBlock)
			{
				return RemainingBytesInBlock(statementOffset);
			}

			int previousInstructionSize = previousInstructionSizes.TryGetValue(instruction, out int knownPreviousInstructionSize)
				? knownPreviousInstructionSize
				: -1;
			int size = opcode.PrefixLength;
			for (int operandIndex = 0; operandIndex < opcode.Operands.Count; operandIndex++)
			{
				GrammarOperand operand = opcode.Operands[operandIndex];
				string token = operandIndex < resolved.Operands.Count ? resolved.Operands[operandIndex] : string.Empty;
				if (operand.Kind == "jump_target_mixed")
				{
					bool forceAbsolute = token.StartsWith("ABS:", StringComparison.OrdinalIgnoreCase);
					bool forceRelative = token.StartsWith("REL:", StringComparison.OrdinalIgnoreCase);
					string targetToken = forceAbsolute || forceRelative ? token[4..] : token;
					if (forceAbsolute)
					{
						size += 3;
					}
					else if (forceRelative)
					{
						size += 1;
					}
					else if (TryResolveCodeOffset(targetToken, module.Name, procedure.Name, previousSymbolOffsets, instruction.LineNumber, out int targetOffset))
					{
						if (targetOffset > statementOffset)
						{
							size += 3;
						}
						else
						{
							int relative = targetOffset - (statementOffset + size + 1);
							size += relative >= -128 && relative <= 127 && relative != 0 ? 1 : 3;
						}
					}
					else if (previousInstructionSize >= 0)
					{
						isProvisional = true;
						return previousInstructionSize;
					}
					else
					{
						isProvisional = true;
						size += 3;
					}
				}
				else
				{
					size += OperandSizeBytes(operand.Kind);
				}
			}

			return size;
		}

		/// <summary>
		/// Tries to encode all procedures in a module to bytes, validating that the encoded sizes match
		/// the planned sizes from the layout pass.
		/// </summary>
		/// <param name="moduleLayout">The module to encode.</param>
		/// <param name="symbols">The finalized symbol catalog.</param>
		/// <param name="encodedInstructionSizes">Updated with the actual encoded size of each instruction.</param>
		/// <param name="moduleBytes">When this method returns <see langword="true"/>, the encoded module bytes.</param>
		/// <param name="failureDetail">When this method returns <see langword="false"/>, a description of the layout mismatch.</param>
		/// <returns><see langword="true"/> if encoding succeeded and all sizes matched; otherwise <see langword="false"/>.</returns>
		private bool TryEncodeModule(
			ModuleLayout moduleLayout,
			SymbolCatalog symbols,
			IDictionary<InstructionDefinition, int> encodedInstructionSizes,
			out byte[] moduleBytes,
			out string? failureDetail)
		{
			List<byte> bytes = new();
			failureDetail = null;
			string? firstSizeMismatch = null;

			foreach (ProcedureLayout procedureLayout in moduleLayout.Procedures)
			{
				if (bytes.Count > procedureLayout.StartOffset)
				{
					failureDetail = $"procedure '{procedureLayout.Procedure.Name}' expected start 0x{procedureLayout.StartOffset:X4}, actual 0x{bytes.Count:X4}. {firstSizeMismatch}";
					moduleBytes = Array.Empty<byte>();
					return false;
				}

				if (bytes.Count < procedureLayout.StartOffset)
				{
					if (procedureLayout.StartOffset - bytes.Count != RemainingBytesInBlock(bytes.Count))
					{
						failureDetail = $"procedure '{procedureLayout.Procedure.Name}' expected pre-header gap 0x{procedureLayout.StartOffset - bytes.Count:X}, remaining block bytes 0x{RemainingBytesInBlock(bytes.Count):X}. {firstSizeMismatch}";
						moduleBytes = Array.Empty<byte>();
						return false;
					}

					bytes.AddRange(EncodeProcedureAlignmentPadding(bytes.Count));
				}

				bytes.AddRange(EncodeProcedureHeader(moduleLayout.Module, procedureLayout.Procedure, symbols));
				foreach (Statement statement in procedureLayout.Procedure.Statements)
				{
					if (statement is LabelStatement)
					{
						continue;
					}

					InstructionDefinition instruction = (InstructionDefinition)statement;
					int statementOffset = procedureLayout.StatementOffsets[statement];
					if (bytes.Count < statementOffset)
					{
						if (statementOffset - bytes.Count != RemainingBytesInBlock(bytes.Count))
						{
							failureDetail = $"line {instruction.LineNumber} expected gap 0x{statementOffset - bytes.Count:X}, remaining block bytes 0x{RemainingBytesInBlock(bytes.Count):X}. {firstSizeMismatch}";
							moduleBytes = Array.Empty<byte>();
							return false;
						}

						bytes.AddRange(EncodeSyntheticNextBlock(bytes.Count));
					}

					int plannedSize = encodedInstructionSizes.TryGetValue(instruction, out int knownPlannedSize)
						? knownPlannedSize
						: -1;
					byte[] encoded;
					try
					{
						encoded = EncodeInstruction(moduleLayout.Module, procedureLayout.Procedure, instruction, statementOffset, symbols, plannedSize);
					}
					catch (AssemblerException exception)
						when (exception.Message.StartsWith("Internal error: line ", StringComparison.Ordinal))
					{
						failureDetail = exception.Message;
						moduleBytes = Array.Empty<byte>();
						return false;
					}

					if (firstSizeMismatch is null
						&& plannedSize >= 0
						&& plannedSize != encoded.Length)
					{
						firstSizeMismatch = $"First size mismatch: line {instruction.LineNumber} planned {plannedSize}, encoded {encoded.Length}.";
					}

					encodedInstructionSizes[instruction] = encoded.Length;
					if (!instruction.Mnemonic.Equals("NEXTB", StringComparison.OrdinalIgnoreCase)
						&& encoded.Length > AvailableBytesBeforeAutoNextBlock(statementOffset))
					{
						throw new AssemblerException(
							$"Line {instruction.LineNumber}: instruction '{instruction.Mnemonic}' is too large for a single 256-byte block.");
					}

					bytes.AddRange(encoded);
				}
			}

			moduleBytes = bytes.ToArray();
			return true;
		}

		/// <summary>Encodes a single instruction to its final byte representation.</summary>
		/// <param name="module">The module containing the instruction.</param>
		/// <param name="procedure">The procedure containing the instruction.</param>
		/// <param name="instruction">The instruction to encode.</param>
		/// <param name="statementOffset">The module-relative byte offset at which this instruction is placed.</param>
		/// <param name="symbols">The finalized symbol catalog for resolving operand values.</param>
		/// <param name="plannedInstructionSize">The size predicted during the layout pass, used to select branch encoding.</param>
		/// <returns>The encoded bytes for this instruction.</returns>
		private byte[] EncodeInstruction(
			ModuleDefinition module,
			ProcedureDefinition procedure,
			InstructionDefinition instruction,
			int statementOffset,
			SymbolCatalog symbols,
			int plannedInstructionSize)
		{
			ResolvedInstruction resolved = ResolveInstructionEncoding(module, procedure, instruction);
			OpcodeInfo opcode = resolved.Opcode;
			List<byte> bytes = new();
			bytes.Add(opcode.Opcode);
			if (opcode.IsExtended)
			{
				bytes.Add(opcode.ExtendedOpcode);
			}

			if (opcode.IsNextBlock)
			{
				int totalLength = RemainingBytesInBlock(statementOffset);
				while (bytes.Count < totalLength)
				{
					bytes.Add(0);
				}

				return bytes.ToArray();
			}

			if (resolved.Operands.Count != opcode.Operands.Count)
			{
				throw new AssemblerException(
					$"Line {instruction.LineNumber}: mnemonic '{instruction.Mnemonic}' expects {opcode.Operands.Count} operand(s), found {resolved.Operands.Count}.");
			}

			for (int operandIndex = 0; operandIndex < opcode.Operands.Count; operandIndex++)
			{
				GrammarOperand operand = opcode.Operands[operandIndex];
				string token = resolved.Operands[operandIndex];
				EncodeOperand(
					bytes,
					operand.Kind,
					token,
					module,
					procedure,
					instruction.LineNumber,
					statementOffset,
					symbols,
					opcode,
					operandIndex,
					plannedInstructionSize);
			}

			return bytes.ToArray();
		}

		/// <summary>
		/// Resolves an instruction mnemonic to a concrete <see cref="OpcodeInfo"/> and its definitive operand token list.
		/// Handles pseudo-mnemonics (PUSH, POP, PUSHL, etc.) by expanding them to whichever real opcode is most compact.
		/// </summary>
		/// <param name="module">The module containing the instruction.</param>
		/// <param name="procedure">The procedure containing the instruction.</param>
		/// <param name="instruction">The instruction to resolve.</param>
		/// <returns>A <see cref="ResolvedInstruction"/> pairing the selected opcode with its operands.</returns>
		private ResolvedInstruction ResolveInstructionEncoding(ModuleDefinition module, ProcedureDefinition procedure, InstructionDefinition instruction)
		{
			string mnemonic = CanonicalizeAssemblyMnemonic(instruction.Mnemonic);

			if (mnemonic.Equals("PUSH", StringComparison.OrdinalIgnoreCase))
			{
				return ResolvePushInstruction(instruction);
			}

			if (mnemonic.Equals("POP", StringComparison.OrdinalIgnoreCase) || mnemonic.Equals("PULL", StringComparison.OrdinalIgnoreCase))
			{
				return ResolvePopInstruction(instruction);
			}

			if (mnemonic.Equals("PUSHL", StringComparison.OrdinalIgnoreCase))
			{
				return ResolvePackedLocalInstruction(procedure, instruction, mnemonic, "PUSH_L");
			}

			if (mnemonic.Equals("PUTL", StringComparison.OrdinalIgnoreCase))
			{
				return ResolvePackedLocalInstruction(procedure, instruction, mnemonic, "PUT_L");
			}

			if (mnemonic.Equals("STOREL", StringComparison.OrdinalIgnoreCase))
			{
				return ResolvePackedLocalInstruction(procedure, instruction, mnemonic, "STORE_L");
			}

			if (mnemonic.Equals("VLOADW", StringComparison.OrdinalIgnoreCase))
			{
				return ResolveVloadWordInstruction(procedure, instruction);
			}

			if (mnemonic.Equals("VLOADB", StringComparison.OrdinalIgnoreCase))
			{
				return ResolveVloadByteInstruction(instruction);
			}

			if (mnemonic.Equals("VPUTW", StringComparison.OrdinalIgnoreCase))
			{
				return ResolveVputWordInstruction(instruction);
			}

			OpcodeInfo opcode = grammar.GetByMnemonic(mnemonic, instruction.LineNumber);
			if (instruction.Operands.Count != opcode.Operands.Count)
			{
				throw new AssemblerException(
					$"Line {instruction.LineNumber}: mnemonic '{instruction.Mnemonic}' expects {opcode.Operands.Count} operand(s), found {instruction.Operands.Count}.");
			}

			return new ResolvedInstruction(opcode, instruction.Operands);
		}

		/// <summary>
		/// Resolves the PUSH pseudo-mnemonic to the most compact real push opcode for the given operand.
		/// Small constants map to zero-operand forms (PUSH0..PUSH8, PUSHFF, etc.); other byte values use PUSHB;
		/// larger values and non-numeric tokens use PUSHW.
		/// </summary>
		/// <param name="instruction">The PUSH instruction to resolve.</param>
		/// <returns>A <see cref="ResolvedInstruction"/> using the selected push opcode.</returns>
		private ResolvedInstruction ResolvePushInstruction(InstructionDefinition instruction)
		{
			if (instruction.Operands.Count != 1)
			{
				throw new AssemblerException(
					$"Line {instruction.LineNumber}: mnemonic '{instruction.Mnemonic}' expects 1 operand, found {instruction.Operands.Count}.");
			}

			string token = instruction.Operands[0];
			if (!TryParseInteger(token, out int numeric))
			{
				OpcodeInfo pushWord = grammar.GetByMnemonic("PUSHW", instruction.LineNumber);
				return new ResolvedInstruction(pushWord, instruction.Operands);
			}

			ushort wordValue = unchecked((ushort)numeric);
			string selectedMnemonic = wordValue switch
			{
				0x0000 => "PUSH0",
				0x0001 => "PUSH1",
				0x0002 => "PUSH2",
				0x0003 => "PUSH3",
				0x0004 => "PUSH4",
				0x0005 => "PUSH5",
				0x0006 => "PUSH6",
				0x0007 => "PUSH7",
				0x0008 => "PUSH8",
				0x00FF => "PUSHFF",
				0xFFF8 => "PUSHm8",
				0xFFFF => "PUSHm1",
				_ when numeric >= 0 && numeric <= 0xFF => "PUSHB",
				_ => "PUSHW",
			};

			OpcodeInfo opcode = grammar.GetByMnemonic(selectedMnemonic, instruction.LineNumber);
			IReadOnlyList<string> operands = opcode.Operands.Count == 0 ? Array.Empty<string>() : instruction.Operands;
			return new ResolvedInstruction(opcode, operands);
		}

		/// <summary>
		/// Resolves the POP/PULL pseudo-mnemonic: POP with no operand maps to the zero-operand POP form;
		/// POP with one operand maps to POPI (pop N items).
		/// </summary>
		/// <param name="instruction">The POP or PULL instruction to resolve.</param>
		/// <returns>A <see cref="ResolvedInstruction"/> using the appropriate pop opcode.</returns>
		private ResolvedInstruction ResolvePopInstruction(InstructionDefinition instruction)
		{
			return instruction.Operands.Count switch
			{
				0 => new ResolvedInstruction(grammar.GetByMnemonic("POP", instruction.LineNumber), Array.Empty<string>()),
				1 => new ResolvedInstruction(grammar.GetByMnemonic("POPI", instruction.LineNumber), instruction.Operands),
				_ => throw new AssemblerException(
					$"Line {instruction.LineNumber}: mnemonic '{instruction.Mnemonic}' expects 0 or 1 operands, found {instruction.Operands.Count}."),
			};
		}

		/// <summary>
		/// Resolves PUSHL, PUTL, or STOREL to the packed single-byte form (e.g. PUSH_L0) when the local index
		/// fits in 0..31, and falls back to the two-byte form otherwise.
		/// </summary>
		/// <param name="procedure">The procedure containing the instruction, used to look up named locals.</param>
		/// <param name="instruction">The instruction to resolve.</param>
		/// <param name="baseMnemonic">The unpacked mnemonic to use as fallback (e.g. "PUSHL").</param>
		/// <param name="packedPrefix">The prefix of the packed mnemonic family (e.g. "PUSH_L").</param>
		/// <returns>A <see cref="ResolvedInstruction"/> using the most compact form available.</returns>
		private ResolvedInstruction ResolvePackedLocalInstruction(ProcedureDefinition procedure, InstructionDefinition instruction, string baseMnemonic, string packedPrefix)
		{
			if (instruction.Operands.Count != 1)
			{
				throw new AssemblerException(
					$"Line {instruction.LineNumber}: mnemonic '{instruction.Mnemonic}' expects 1 operand, found {instruction.Operands.Count}.");
			}

			if (TryResolveLocalIndexToken(procedure, instruction.Operands[0], out int numericLocal) && numericLocal >= 0 && numericLocal <= 31)
			{
				return new ResolvedInstruction(
					grammar.GetByMnemonic($"{packedPrefix}{numericLocal}", instruction.LineNumber),
					Array.Empty<string>());
			}

			return new ResolvedInstruction(grammar.GetByMnemonic(baseMnemonic, instruction.LineNumber), instruction.Operands);
		}

		/// <summary>
		/// Resolves the VLOADW pseudo-mnemonic to the appropriate real opcode:
		/// no operands → VLOADW, one operand → VLOADW_, two operands → packed VREAD form (if indices fit).
		/// </summary>
		/// <param name="procedure">The procedure containing the instruction, used for local name resolution.</param>
		/// <param name="instruction">The VLOADW instruction to resolve.</param>
		/// <returns>A <see cref="ResolvedInstruction"/> using the appropriate vector-load opcode.</returns>
		private ResolvedInstruction ResolveVloadWordInstruction(ProcedureDefinition procedure, InstructionDefinition instruction)
		{
			switch (instruction.Operands.Count)
			{
				case 0:
					return new ResolvedInstruction(grammar.GetByMnemonic("VLOADW", instruction.LineNumber), Array.Empty<string>());
				case 1:
					return new ResolvedInstruction(grammar.GetByMnemonic("VLOADW_", instruction.LineNumber), instruction.Operands);
				case 2:
				{
					if (TryParseInteger(instruction.Operands[0], out int wordIndex)
						&& TryResolveLocalIndexToken(procedure, instruction.Operands[1], out int localIndex)
						&& wordIndex >= 0 && wordIndex <= 3
						&& localIndex >= 0 && localIndex <= 15)
					{
						return new ResolvedInstruction(
							grammar.GetByMnemonic($"VREAD_{localIndex}_{wordIndex}", instruction.LineNumber),
							Array.Empty<string>());
					}

					throw new AssemblerException(
						$"Line {instruction.LineNumber}: mnemonic '{instruction.Mnemonic}' with two operands requires word index 0..3 and local index 0..15 so it can use the packed VREAD form.");
				}
				default:
					throw new AssemblerException(
						$"Line {instruction.LineNumber}: mnemonic '{instruction.Mnemonic}' expects 0, 1, or 2 operands, found {instruction.Operands.Count}.");
			}
		}

		/// <summary>
		/// Resolves the VLOADB pseudo-mnemonic: no operands → VLOADB; one operand → VLOADB_.
		/// </summary>
		/// <param name="instruction">The VLOADB instruction to resolve.</param>
		/// <returns>A <see cref="ResolvedInstruction"/> using the appropriate vector byte-load opcode.</returns>
		private ResolvedInstruction ResolveVloadByteInstruction(InstructionDefinition instruction)
		{
			return instruction.Operands.Count switch
			{
				0 => new ResolvedInstruction(grammar.GetByMnemonic("VLOADB", instruction.LineNumber), Array.Empty<string>()),
				1 => new ResolvedInstruction(grammar.GetByMnemonic("VLOADB_", instruction.LineNumber), instruction.Operands),
				_ => throw new AssemblerException(
					$"Line {instruction.LineNumber}: mnemonic '{instruction.Mnemonic}' expects 0 or 1 operands, found {instruction.Operands.Count}."),
			};
		}

		/// <summary>
		/// Resolves the VPUTW pseudo-mnemonic: no operands → VPUTW; one operand → VPUTW_.
		/// </summary>
		/// <param name="instruction">The VPUTW instruction to resolve.</param>
		/// <returns>A <see cref="ResolvedInstruction"/> using the appropriate vector word-store opcode.</returns>
		private ResolvedInstruction ResolveVputWordInstruction(InstructionDefinition instruction)
		{
			return instruction.Operands.Count switch
			{
				0 => new ResolvedInstruction(grammar.GetByMnemonic("VPUTW", instruction.LineNumber), Array.Empty<string>()),
				1 => new ResolvedInstruction(grammar.GetByMnemonic("VPUTW_", instruction.LineNumber), instruction.Operands),
				_ => throw new AssemblerException(
					$"Line {instruction.LineNumber}: mnemonic '{instruction.Mnemonic}' expects 0 or 1 operands, found {instruction.Operands.Count}."),
			};
		}

		/// <summary>
		/// Maps legacy and alias mnemonics to their canonical names, so the rest of the assembler only needs
		/// to handle canonical forms.
		/// </summary>
		/// <param name="mnemonic">The mnemonic as written in the source.</param>
		/// <returns>The canonical mnemonic name.</returns>
		private static string CanonicalizeAssemblyMnemonic(string mnemonic) => mnemonic.ToUpperInvariant() switch
		{
			"ADD_" => "REST",
			"PULL" => "POP",
			"PULL_" => "POPI",
			"PULLN" => "POPN",
			"PULLRET" => "POPRET",
			"RET_" => "RETN",
			"VECL" => "ADVANCE",
			"JUMPL2" => "JUMPL",
			"JUMPLE2" => "JUMPLE",
			"JUMPGE2" => "JUMPGE",
			"JUMPG2" => "JUMPG",
			"LOADL" => "PUSHL",
			"POPVB2" => "PUTVB2",
			_ => mnemonic,
		};

		/// <summary>Tries to resolve a local variable token to a numeric index, by numeric parse or by name lookup.</summary>
		/// <param name="procedure">The procedure whose local table to search.</param>
		/// <param name="token">The local variable token to resolve.</param>
		/// <param name="localIndex">When this method returns <see langword="true"/>, the 0-based local index.</param>
		/// <returns><see langword="true"/> if the token resolved to a local index; otherwise <see langword="false"/>.</returns>
		private static bool TryResolveLocalIndexToken(ProcedureDefinition procedure, string token, out int localIndex)
		{
			if (TryParseInteger(token, out localIndex))
			{
				return true;
			}

			return procedure.LocalNames.TryGetValue(token, out localIndex);
		}

		/// <summary>
		/// Encodes a NEXTB instruction padded with zero fill bytes to consume the remainder of the current 256-byte block.
		/// This is used when the assembler automatically inserts a NEXTB before an instruction that would otherwise
		/// straddle a block boundary.
		/// </summary>
		/// <param name="statementOffset">The module-relative offset where the NEXTB will be placed.</param>
		/// <returns>The encoded NEXTB bytes.</returns>
		private byte[] EncodeSyntheticNextBlock(int statementOffset)
		{
			OpcodeInfo nextBlock = grammar.GetByMnemonic("NEXTB", 0);
			List<byte> bytes = new();
			bytes.Add(nextBlock.Opcode);
			if (nextBlock.IsExtended)
			{
				bytes.Add(nextBlock.ExtendedOpcode);
			}

			int totalLength = RemainingBytesInBlock(statementOffset);
			if (bytes.Count > totalLength)
			{
				throw new AssemblerException("Internal error: NEXTB prefix is larger than remaining block bytes.");
			}

			while (bytes.Count < totalLength)
			{
				bytes.Add(0);
			}

			return bytes.ToArray();
		}

		/// <summary>Encodes zero-fill bytes to advance past the remainder of a 256-byte block, aligning the next procedure to a block boundary.</summary>
		/// <param name="procedureStartOffset">The current offset, which is the start of the padding region.</param>
		/// <returns>A zero-filled byte array covering the remainder of the current block.</returns>
		private static byte[] EncodeProcedureAlignmentPadding(int procedureStartOffset)
		{
			int totalLength = RemainingBytesInBlock(procedureStartOffset);
			byte[] bytes = new byte[totalLength];
			return bytes;
		}

		/// <summary>
		/// Returns <see langword="true"/> if this instruction needs a NEXTB inserted before it because
		/// it would not fit entirely within the remaining bytes of the current 256-byte block.
		/// NEXTB instructions themselves are never subject to this rule.
		/// </summary>
		/// <param name="instruction">The instruction being considered.</param>
		/// <param name="statementOffset">The offset at which the instruction would be placed.</param>
		/// <param name="instructionSize">The estimated byte size of the instruction.</param>
		/// <returns><see langword="true"/> if a NEXTB should be inserted before this instruction.</returns>
		private static bool ShouldAutoInsertNextBlock(InstructionDefinition instruction, int statementOffset, int instructionSize)
		{
			return !instruction.Mnemonic.Equals("NEXTB", StringComparison.OrdinalIgnoreCase)
				&& instructionSize > AvailableBytesBeforeAutoNextBlock(statementOffset);
		}

		/// <summary>Encodes a single instruction operand and appends its bytes to the instruction byte list.</summary>
		/// <param name="bytes">The byte list being built for the current instruction.</param>
		/// <param name="operandKind">The grammar-defined encoding kind for this operand position.</param>
		/// <param name="token">The source token for this operand.</param>
		/// <param name="module">The module containing the instruction.</param>
		/// <param name="procedure">The procedure containing the instruction.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <param name="statementOffset">The module-relative offset of the instruction, used for relative branch calculation.</param>
		/// <param name="symbols">The symbol catalog for resolving operand values.</param>
		/// <param name="opcode">The opcode being encoded, used to compute trailing operand sizes for branch instructions.</param>
		/// <param name="operandIndex">The 0-based index of this operand within the instruction.</param>
		/// <param name="plannedInstructionSize">The size predicted during layout, used to select short vs. long branch encoding.</param>
		private void EncodeOperand(
			List<byte> bytes,
			string operandKind,
			string token,
			ModuleDefinition module,
			ProcedureDefinition procedure,
			int lineNumber,
			int statementOffset,
			SymbolCatalog symbols,
			OpcodeInfo opcode,
			int operandIndex,
			int plannedInstructionSize)
		{
			switch (operandKind)
			{
						case "immediate_u8":
				case "aggregate_byte_index_u8":
				case "aggregate_word_index_u8":
				case "argument_count_u8":
				case "open_mode_u8":
				case "return_count_u8":
				case "stack_drop_count_u8":
				case "display_subop_u8":
				case "display_ext_subop_u8":
					bytes.Add(EvaluateByteOperand(token, lineNumber, symbols));
					break;

					case "local_index_u8":
						bytes.Add(symbols.ResolveLocalIndex(token, module.Name, procedure.Name, lineNumber));
						break;

					case "program_global_index_u8":
						bytes.Add(symbols.ResolveProgramGlobalIndex(token, lineNumber));
						break;

					case "module_global_index_u8":
						bytes.Add(symbols.ResolveModuleGlobalIndex(token, module.Name, lineNumber));
						break;

				case "immediate_u16le":
				case "status_code_u16le":
				case "bitfield_control_u16le":
					WriteWord(bytes, EvaluateWordOperand(token, lineNumber, symbols));
					break;

				case "near_code_offset_u16le":
				{
					int target = symbols.ResolveCodeOffset(token, module.Name, procedure.Name, lineNumber, requireSameModule: true);
					WriteWord(bytes, checked((ushort)target));
					break;
				}
				case "packed_code_location_u16le":
				{
					int target = symbols.ResolveCodeOffset(token, module.Name, procedure.Name, lineNumber, requireSameModule: true);
					WriteWord(bytes, checked((ushort)target));
					break;
				}
				case "far_proc_selector_u16le":
				{
					ushort selector;
					if (TryParseInteger(token, out int numericSelector))
					{
						selector = checked((ushort)numericSelector);
					}
					else
					{
						ProcedureSymbol procedureSymbol = symbols.ResolveProcedureReference(token, module.Name, procedure.Name, lineNumber);
						if (procedureSymbol.ProcedureIndex < 0)
						{
							throw new AssemblerException($"Line {lineNumber}: far procedure reference '{token}' targets a private procedure; add an .export directive or use a numeric selector.");
						}

						selector = checked((ushort)((procedureSymbol.ModuleId << 8) | procedureSymbol.ProcedureIndex));
					}

					WriteWord(bytes, selector);
					break;
				}
				case "jump_target_mixed":
				{
					bool forceAbsolute = token.StartsWith("ABS:", StringComparison.OrdinalIgnoreCase);
					bool forceRelative = token.StartsWith("REL:", StringComparison.OrdinalIgnoreCase);
					string targetToken = forceAbsolute || forceRelative ? token[4..] : token;
					int target = symbols.ResolveCodeOffset(targetToken, module.Name, procedure.Name, lineNumber, requireSameModule: true);
					int relative = target - (statementOffset + bytes.Count + 1);
					int trailingOperandSize = GetFixedTrailingOperandSize(opcode, operandIndex);
					int shortInstructionSize = bytes.Count + 1 + trailingOperandSize;
					int longInstructionSize = bytes.Count + 3 + trailingOperandSize;
					if (forceRelative)
					{
						if (relative < -128 || relative > 127 || relative == 0)
						{
							throw new AssemblerException($"Line {lineNumber}: REL target '{targetToken}' is out of short-jump range.");
						}

						bytes.Add(unchecked((byte)(sbyte)relative));
					}
					else if (plannedInstructionSize == shortInstructionSize)
					{
						if (relative < -128 || relative > 127 || relative == 0)
						{
							throw new AssemblerException($"Internal error: line {lineNumber} planned a short jump, but target '{targetToken}' no longer fits.");
						}

						bytes.Add(unchecked((byte)(sbyte)relative));
					}
					else if (plannedInstructionSize == longInstructionSize)
					{
						bytes.Add(0);
						WriteWord(bytes, checked((ushort)target));
					}
					else if (!forceAbsolute && target <= statementOffset && relative >= -128 && relative <= 127 && relative != 0)
					{
						bytes.Add(unchecked((byte)(sbyte)relative));
					}
					else
					{
						bytes.Add(0);
						WriteWord(bytes, checked((ushort)target));
					}

					break;
				}
				default:
					throw new AssemblerException($"Line {lineNumber}: unsupported operand kind '{operandKind}'.");
			}
		}

		/// <summary>
		/// Constructs the definitive <see cref="SymbolCatalog"/> after all module layouts are finalized,
		/// recording the correct module-relative start offset for each procedure.
		/// </summary>
		/// <param name="dataSymbols">The combined data symbol table.</param>
		/// <param name="moduleLayouts">The finalized module layouts.</param>
		/// <returns>A fully resolved <see cref="SymbolCatalog"/> for use during instruction encoding.</returns>
		private SymbolCatalog BuildFinalSymbols(IReadOnlyDictionary<string, DataSymbol> dataSymbols, IReadOnlyList<ModuleLayout> moduleLayouts)
		{
			Dictionary<string, DataSymbol> dataByName = new(dataSymbols, StringComparer.OrdinalIgnoreCase);
			Dictionary<string, ProcedureSymbol> procedures = new(StringComparer.OrdinalIgnoreCase);
			Dictionary<string, CodeLabelSymbol> labels = new(StringComparer.OrdinalIgnoreCase);
			Dictionary<string, ConstantDefinition> constants = new(source.Constants, StringComparer.OrdinalIgnoreCase);
			Dictionary<string, int> programGlobals = new(source.ProgramGlobalNames, StringComparer.OrdinalIgnoreCase);
			Dictionary<string, Dictionary<string, int>> moduleGlobals = new(StringComparer.OrdinalIgnoreCase);
			Dictionary<string, Dictionary<string, int>> locals = new(StringComparer.OrdinalIgnoreCase);

			foreach (ModuleLayout moduleLayout in moduleLayouts)
			{
				moduleGlobals[moduleLayout.Module.Name] = new Dictionary<string, int>(moduleLayout.Module.ModuleGlobalNames, StringComparer.OrdinalIgnoreCase);

				foreach (ProcedureLayout procedureLayout in moduleLayout.Procedures)
				{
					locals[QualifyProcedure(moduleLayout.Module.Name, procedureLayout.Procedure.Name)] = new Dictionary<string, int>(procedureLayout.Procedure.LocalNames, StringComparer.OrdinalIgnoreCase);
					procedures[QualifyProcedure(moduleLayout.Module.Name, procedureLayout.Procedure.Name)] = new ProcedureSymbol(
						moduleLayout.Module.Name,
						procedureLayout.Procedure.Name,
						moduleLayout.Module.ModuleId,
						procedureLayout.Procedure.ProcedureIndex,
						procedureLayout.StartOffset,
						procedureLayout.Procedure.LineNumber);

					foreach ((string labelName, int offset) in procedureLayout.LocalLabels)
					{
						labels[QualifyLabel(moduleLayout.Module.Name, procedureLayout.Procedure.Name, labelName)] = new CodeLabelSymbol(
							moduleLayout.Module.Name,
							procedureLayout.Procedure.Name,
							labelName,
							offset,
							procedureLayout.Procedure.LineNumber);
					}
				}
			}

			return new SymbolCatalog(dataByName, procedures, labels, constants, programGlobals, moduleGlobals, locals);
		}

		/// <summary>
		/// Combines all module bytecode into a contiguous block, pads each module boundary to a 256-byte page,
		/// and appends the object-data section to produce the complete OBJ file bytes.
		/// </summary>
		/// <param name="moduleLayouts">The laid-out modules whose encoded bytes are appended in order.</param>
		/// <param name="objDataBytes">The encoded object-data section bytes to append after the code.</param>
		/// <returns>An <see cref="ObjectImageLayout"/> containing the full byte array and the length of the code-only portion.</returns>
		private ObjectImageLayout BuildObjectImage(IReadOnlyList<ModuleLayout> moduleLayouts, byte[] objDataBytes)
		{
			List<byte> bytes = new();
			int[] moduleOffsets = new int[moduleLayouts.Count];
			for (int index = 0; index < moduleLayouts.Count; index++)
			{
				ModuleLayout layout = moduleLayouts[index];
				moduleOffsets[index] = bytes.Count;
				layout.ObjectOffset = bytes.Count;
				bytes.AddRange(layout.Bytes ?? Array.Empty<byte>());
				if (index != moduleLayouts.Count - 1)
				{
					while ((bytes.Count & 0xFF) != 0)
					{
						bytes.Add(0);
					}
				}
			}

			while ((bytes.Count & 0x1FF) != 0)
			{
				bytes.Add(0);
			}

			int codeByteLength = bytes.Count;
			bytes.AddRange(objDataBytes);

			for (int index = 0; index < moduleLayouts.Count; index++)
			{
				moduleLayouts[index].ObjectOffset = moduleOffsets[index];
			}

			return new ObjectImageLayout(bytes.ToArray(), codeByteLength);
		}

		/// <summary>
		/// Builds the complete MME metadata image, including the fixed header, module and global tables,
		/// module procedure headers, and the initial RAM data.
		/// </summary>
		/// <param name="ramLayout">The initial RAM layout.</param>
		/// <param name="moduleLayouts">The finalized module layouts.</param>
		/// <param name="objectByteLength">The total length of the OBJ file in bytes, used to compute the code page count.</param>
		/// <param name="symbols">The finalized symbol catalog, used to evaluate global initializer values.</param>
		/// <returns>The encoded MME metadata bytes.</returns>
		private byte[] BuildMetadataImage(RamLayout ramLayout, IReadOnlyList<ModuleLayout> moduleLayouts, int objectByteLength, SymbolCatalog symbols)
		{
			ProcedureSymbol entryProcedure = symbols.ResolveProcedureReference(source.EntrySymbol, null, null, source.EntryLine);
			if (entryProcedure.ProcedureIndex < 0)
			{
				throw new AssemblerException($"Line {source.EntryLine}: entry procedure '{source.EntrySymbol}' is private; entry must reference an exported procedure.");
			}

			int[] moduleOffsets = moduleLayouts.Select(layout => layout.ObjectOffset).ToArray();
			int[] moduleOffsetPages = moduleOffsets.Select(offset => offset / 0x100).ToArray();
			ushort[] headerWords = new ushort[HeaderWordCount];
			Array.Copy(HeaderPreambleWords, headerWords, HeaderPreambleWords.Length);
			headerWords[7] = checked((ushort)((objectByteLength / 0x200) - 1));
			headerWords[8] = checked((ushort)((entryProcedure.ModuleId << 8) | entryProcedure.ProcedureIndex));
			headerWords[11] = checked((ushort)ramLayout.InitialRamWords);

			int moduleCount = moduleLayouts.Count;
			int preHeaderBytes = HeaderBytes
				+ 2
				+ moduleCount * 2
				+ 2
				+ moduleCount * 2
				+ source.ProgramGlobals.Count * 2
				+ source.Modules.Sum(module => module.ModuleGlobals.Count * 2);

			int moduleHeadersPage = (preHeaderBytes + 0xFF) / 0x100;
			headerWords[13] = checked((ushort)moduleHeadersPage);

			List<byte> moduleHeaders = new();
			foreach (ModuleLayout moduleLayout in moduleLayouts)
			{
				List<ProcedureLayout> exportedProcedures = moduleLayout.Procedures
					.Where(procedure => procedure.Procedure.ProcedureIndex >= 0)
					.OrderBy(procedure => procedure.Procedure.ProcedureIndex)
					.ToList();

				WriteWord(moduleHeaders, checked((ushort)exportedProcedures.Count));
				foreach (ProcedureLayout procedure in exportedProcedures)
				{
					WriteWord(moduleHeaders, checked((ushort)procedure.StartOffset));
				}
			}

			headerWords[15] = checked((ushort)(moduleHeaders.Count / 2));
			int initialRamOffset = 0x100 * (1 + moduleHeadersPage + (moduleHeaders.Count / 0x100));

			List<byte> metadata = new(HeaderBytes + 2048 + ramLayout.Bytes.Length);
			foreach (ushort word in headerWords)
			{
				WriteWord(metadata, word);
			}

			WriteWord(metadata, checked((ushort)moduleCount));
			foreach (int page in moduleOffsetPages)
			{
				WriteWord(metadata, checked((ushort)page));
			}

			WriteWord(metadata, checked((ushort)source.ProgramGlobals.Count));
			foreach (ModuleDefinition module in source.Modules)
			{
				WriteWord(metadata, checked((ushort)module.ModuleGlobals.Count));
			}

			foreach (string token in source.ProgramGlobals)
			{
				WriteWord(metadata, EvaluateWordOperand(token, source.ProgramGlobalsLine ?? 0, symbols));
			}

			foreach (ModuleDefinition module in source.Modules)
			{
				foreach (string token in module.ModuleGlobals)
				{
					WriteWord(metadata, EvaluateWordOperand(token, module.ModuleGlobalsLine ?? module.LineNumber, symbols));
				}
			}

			while (metadata.Count < moduleHeadersPage * 0x100)
			{
				metadata.Add(0);
			}

			metadata.AddRange(moduleHeaders);
			while (metadata.Count < initialRamOffset)
			{
				metadata.Add(0);
			}

			metadata.AddRange(ramLayout.Bytes);
			return metadata.ToArray();
		}

		/// <summary>
		/// Encodes the per-procedure header that precedes the procedure's code: a local-count byte followed by
		/// zero or more initializer entries.
		/// </summary>
		/// <param name="module">The module containing the procedure (unused, reserved for future use).</param>
		/// <param name="procedure">The procedure whose header is being encoded.</param>
		/// <param name="symbols">The symbol catalog used to evaluate initializer value tokens.</param>
		/// <returns>The encoded header bytes.</returns>
		private static byte[] EncodeProcedureHeader(ModuleDefinition module, ProcedureDefinition procedure, SymbolCatalog symbols)
		{
			List<byte> bytes = new();
			byte header = checked((byte)(procedure.LocalCount & 0x7F));
			if (procedure.Initializers.Count > 0)
			{
				header |= 0x80;
			}

			bytes.Add(header);
			for (int index = 0; index < procedure.Initializers.Count; index++)
			{
				LocalInitializer initializer = procedure.Initializers[index];
				ushort resolvedValue = EvaluateWordOperand(initializer.ValueToken, initializer.LineNumber, symbols);
				bool encodeAsByte = ShouldEncodeInitializerAsByte(resolvedValue);
				byte marker = checked((byte)(initializer.LocalIndex & 0x3F));
				if (encodeAsByte)
				{
					marker |= 0x40;
				}

				if (index == procedure.Initializers.Count - 1)
				{
					marker |= 0x80;
				}

				bytes.Add(marker);
				if (encodeAsByte)
				{
					bytes.Add(unchecked((byte)(sbyte)(short)resolvedValue));
				}
				else
				{
					WriteWord(bytes, resolvedValue);
				}
			}

			return bytes.ToArray();
		}

		/// <summary>
		/// Computes the byte size of a procedure's header to determine whether it fits within the current
		/// 256-byte block or requires alignment padding.
		/// </summary>
		/// <param name="module">The module containing the procedure.</param>
		/// <param name="procedure">The procedure whose header size is computed.</param>
		/// <param name="symbols">The symbol catalog, used to evaluate initializer values for byte-vs-word encoding decisions.</param>
		/// <returns>The number of bytes the procedure header will occupy.</returns>
		private static int GetProcedureHeaderSize(ModuleDefinition module, ProcedureDefinition procedure, SymbolCatalog symbols)
		{
			int size = 1;
			foreach (LocalInitializer initializer in procedure.Initializers)
			{
				size += 1;
				ushort resolvedValue = EvaluateWordOperand(initializer.ValueToken, initializer.LineNumber, symbols);
				size += ShouldEncodeInitializerAsByte(resolvedValue) ? 1 : 2;
			}

			return size;
		}

		/// <summary>
		/// Returns <see langword="true"/> if an initializer value can be encoded as a signed byte,
		/// saving one byte compared to the full 16-bit encoding.
		/// </summary>
		/// <param name="value">The 16-bit value to test.</param>
		/// <returns><see langword="true"/> if the value is in the range -128..127.</returns>
		private static bool ShouldEncodeInitializerAsByte(ushort value)
		{
			short signed = unchecked((short)value);
			return signed >= sbyte.MinValue && signed <= sbyte.MaxValue;
		}

		/// <summary>Returns the byte length of a data directive's payload (not including any alignment padding).</summary>
		/// <param name="directive">The data directive to measure.</param>
		/// <returns>The number of payload bytes this directive contributes.</returns>
		private static int GetDataByteLength(DataDirective directive)
		{
			return directive.Kind switch
			{
				DataDirectiveKind.String => 2 + GetAsciiBytes(directive.StringValue!, directive.LineNumber).Length,
				DataDirectiveKind.Words => directive.Values.Count * 2,
				DataDirectiveKind.Bytes => directive.Values.Count,
				_ => throw new AssemblerException($"Line {directive.LineNumber}: unsupported RAM directive kind '{directive.Kind}'."),
			};
		}

		/// <summary>Encodes a sequence of data directives to bytes, padding the result to an even length.</summary>
		/// <param name="directives">The data directives to encode, in order.</param>
		/// <param name="symbols">The symbol catalog used to evaluate word and byte operand tokens.</param>
		/// <returns>The encoded bytes, padded to an even length.</returns>
		private static byte[] EncodeDataBlock(IReadOnlyList<DataDirective> directives, SymbolCatalog symbols)
		{
			List<byte> bytes = new();
			foreach (DataDirective directive in directives)
			{
				bytes.AddRange(EncodeDataDirective(directive, symbols));
			}

			while ((bytes.Count & 1) != 0)
			{
				bytes.Add(0);
			}

			return bytes.ToArray();
		}

		/// <summary>Encodes a single data directive (<c>.string</c>, <c>.word</c>/<c>.words</c>, or <c>.byte</c>/<c>.bytes</c>) to bytes.</summary>
		/// <param name="directive">The directive to encode.</param>
		/// <param name="symbols">The symbol catalog used to evaluate operand tokens.</param>
		/// <returns>The encoded bytes for this directive (without alignment padding).</returns>
		private static byte[] EncodeDataDirective(DataDirective directive, SymbolCatalog symbols)
		{
			List<byte> bytes = new();
			switch (directive.Kind)
			{
				case DataDirectiveKind.String:
				{
					byte[] text = GetAsciiBytes(directive.StringValue!, directive.LineNumber);
					WriteWord(bytes, checked((ushort)text.Length));
					bytes.AddRange(text);
					break;
				}
				case DataDirectiveKind.Words:
					foreach (string token in directive.Values)
					{
						WriteWord(bytes, EvaluateWordOperand(token, directive.LineNumber, symbols));
					}

					break;
				case DataDirectiveKind.Bytes:
					foreach (string token in directive.Values)
					{
						bytes.Add(EvaluateByteOperand(token, directive.LineNumber, symbols, allowDataSymbols: true));
					}

					break;
			}

			return bytes.ToArray();
		}

		/// <summary>Converts a string to ASCII bytes, throwing an error if it contains any non-ASCII characters.</summary>
		/// <param name="text">The string to encode.</param>
		/// <param name="lineNumber">The source line number, used in error messages.</param>
		/// <returns>The ASCII-encoded bytes.</returns>
		private static byte[] GetAsciiBytes(string text, int lineNumber)
		{
			try
			{
				Encoder encoder = Encoding.ASCII.GetEncoder();
				encoder.Fallback = EncoderFallback.ExceptionFallback;
				byte[] bytes = new byte[Encoding.ASCII.GetByteCount(text)];
				encoder.GetBytes(text.ToCharArray(), 0, text.Length, bytes, 0, true);
				return bytes;
			}
			catch (EncoderFallbackException)
			{
				throw new AssemblerException($"Line {lineNumber}: string literals must be ASCII.");
			}
		}

		/// <summary>
		/// Tries to resolve a code target token to a module-relative byte offset using only the symbol offsets
		/// from a previous layout pass. Unlike <see cref="SymbolCatalog.ResolveCodeOffset"/>, this method does not
		/// throw on failure; it returns <see langword="false"/> instead, so the caller can use a provisional size.
		/// </summary>
		/// <param name="token">The target token (label, procedure name, or numeric literal).</param>
		/// <param name="moduleName">The name of the enclosing module.</param>
		/// <param name="procedureName">The name of the enclosing procedure.</param>
		/// <param name="offsets">Symbol offsets from a previous layout pass.</param>
		/// <param name="lineNumber">The source line number (currently unused, reserved for future error messages).</param>
		/// <param name="targetOffset">When this method returns <see langword="true"/>, the resolved offset.</param>
		/// <returns><see langword="true"/> if the token was resolved; otherwise <see langword="false"/>.</returns>
		private static bool TryResolveCodeOffset(string token, string moduleName, string procedureName, IReadOnlyDictionary<string, int> offsets, int lineNumber, out int targetOffset)
		{
			if (TryParseInteger(token, out int numeric))
			{
				targetOffset = numeric;
				return true;
			}

			if (offsets.TryGetValue(QualifyLabel(moduleName, procedureName, token), out targetOffset))
			{
				return true;
			}

			if (offsets.TryGetValue(QualifyProcedure(moduleName, token), out targetOffset))
			{
				return true;
			}

			if (offsets.TryGetValue(token, out targetOffset))
			{
				return true;
			}

			targetOffset = 0;
			return false;
		}
	}
}
