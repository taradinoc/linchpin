/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Globalization;
using System.Text.RegularExpressions;

namespace Linchpin;

/// <summary>
/// Provides human-readable procedure names for trace output by loading symbols from
/// <c>.cas</c> disassembly files and <c>.sym</c> symbol table files.
/// </summary>
internal sealed partial class VmProcedureSymbolCatalog
{
	private readonly Dictionary<VmProcedureTarget, string> names;

	private VmProcedureSymbolCatalog(Dictionary<VmProcedureTarget, string> names)
	{
		this.names = names;
	}

	/// <summary>
	/// Loads procedure symbols from configured paths, the image's companion <c>.sym</c> file,
	/// and any matching <c>.cas</c> files in the artifacts directory.
	/// </summary>
	/// <param name="image">The loaded Cornerstone image, used to determine companion file paths and module list.</param>
	/// <param name="repositoryRoot">The repository root directory for locating the artifacts folder.</param>
	/// <param name="configuredPaths">Explicitly configured symbol file paths.</param>
	/// <returns>A catalog mapping procedure targets to their symbolic names.</returns>
	public static VmProcedureSymbolCatalog Load(CornerstoneImage image, string repositoryRoot, IReadOnlyList<string> configuredPaths)
	{
		Dictionary<VmProcedureTarget, string> names = new();
		foreach (string path in ResolveCandidatePaths(image, repositoryRoot, configuredPaths))
		{
			string extension = Path.GetExtension(path);
			if (extension.Equals(".cas", StringComparison.OrdinalIgnoreCase))
			{
				LoadCasFile(path, names);
				continue;
			}

			if (extension.Equals(".sym", StringComparison.OrdinalIgnoreCase))
			{
				LoadSymbolFile(path, names);
			}
		}

		return new VmProcedureSymbolCatalog(names);
	}

	/// <summary>
	/// Attempts to find a symbolic name for a procedure at the given module and start offset.
	/// </summary>
	public bool TryGetName(int moduleId, int startOffset, out string? name)
	{
		return names.TryGetValue(new VmProcedureTarget(moduleId, startOffset), out name);
	}

	private static IReadOnlyList<string> ResolveCandidatePaths(
		CornerstoneImage image,
		string repositoryRoot,
		IReadOnlyList<string> configuredPaths)
	{
		List<string> paths = new();
		HashSet<string> seen = new(StringComparer.OrdinalIgnoreCase);

		void AddPath(string path, bool required)
		{
			string fullPath = Path.GetFullPath(path);
			if (!File.Exists(fullPath))
			{
				if (required)
				{
					throw new LinchpinException($"Symbol file '{fullPath}' does not exist.");
				}

				return;
			}

			if (seen.Add(fullPath))
			{
				paths.Add(fullPath);
			}
		}

		foreach (string configuredPath in configuredPaths)
		{
			AddPath(configuredPath, required: true);
		}

		AddPath(Path.ChangeExtension(image.ObjPath, ".sym"), required: false);

		string artifactsPath = Path.Combine(repositoryRoot, "artifacts");
		if (Directory.Exists(artifactsPath))
		{
			foreach (ModuleImage module in image.Modules)
			{
				AddPath(Path.Combine(artifactsPath, $"Module{module.ModuleId:D2}.cas"), required: false);
			}
		}

		return paths;
	}

	private static void LoadCasFile(string path, Dictionary<VmProcedureTarget, string> names)
	{
		int? currentModuleId = null;
		string? pendingProcedureName = null;

		foreach (string line in File.ReadLines(path))
		{
			string trimmed = line.Trim();
			if (trimmed.Length == 0)
			{
				continue;
			}

			Match moduleMatch = CasModuleRegex().Match(trimmed);
			if (moduleMatch.Success)
			{
				currentModuleId = int.Parse(moduleMatch.Groups["module"].Value, CultureInfo.InvariantCulture);
				pendingProcedureName = null;
				continue;
			}

			Match procedureMatch = CasProcedureRegex().Match(trimmed);
			if (procedureMatch.Success)
			{
				pendingProcedureName = procedureMatch.Groups["name"].Value;
				continue;
			}

			if (!currentModuleId.HasValue || string.IsNullOrWhiteSpace(pendingProcedureName))
			{
				continue;
			}

			Match offsetMatch = CasOffsetsRegex().Match(trimmed);
			if (!offsetMatch.Success)
			{
				continue;
			}

			int startOffset = int.Parse(offsetMatch.Groups["start"].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
			names[new VmProcedureTarget(currentModuleId.Value, startOffset)] = pendingProcedureName;
			pendingProcedureName = null;
		}
	}

	private static void LoadSymbolFile(string path, Dictionary<VmProcedureTarget, string> names)
	{
		foreach (string line in File.ReadLines(path))
		{
			string trimmed = line.Trim();
			if (trimmed.Length == 0 || trimmed.StartsWith('#') || trimmed.StartsWith(';'))
			{
				continue;
			}

			(string moduleToken, string startOffsetToken, string name) = ParseSymbolLine(path, trimmed);
			int moduleId = ParseSymbolInteger(path, moduleToken);
			int startOffset = ParseSymbolInteger(path, startOffsetToken);
			if (moduleId <= 0 || startOffset < 0)
			{
				throw new LinchpinException($"Symbol entry '{trimmed}' in '{path}' has an invalid module or start offset.");
			}

			names[new VmProcedureTarget(moduleId, startOffset)] = name;
		}
	}

	private static (string ModuleToken, string StartOffsetToken, string Name) ParseSymbolLine(string path, string line)
	{
		string normalized = line.Replace(',', ' ');
		string[] parts = normalized.Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
		if (parts.Length == 0)
		{
			throw new LinchpinException($"Symbol line '{line}' in '{path}' is empty.");
		}

		if (parts[0].Contains(':', StringComparison.Ordinal))
		{
			string[] targetParts = parts[0].Split(':', 2, StringSplitOptions.TrimEntries);
			if (targetParts.Length != 2 || parts.Length < 2)
			{
				throw new LinchpinException($"Symbol line '{line}' in '{path}' must include a symbol name.");
			}

			return (targetParts[0], targetParts[1], string.Join(' ', parts.Skip(1)));
		}

		if (parts.Length < 3)
		{
			throw new LinchpinException($"Symbol line '{line}' in '{path}' must look like 'module:start name' or 'module start name'.");
		}

		return (parts[0], parts[1], string.Join(' ', parts.Skip(2)));
	}

	private static int ParseSymbolInteger(string path, string token)
	{
		try
		{
			return token.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
				? int.Parse(token[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture)
				: int.Parse(token, CultureInfo.InvariantCulture);
		}
		catch (FormatException)
		{
			throw new LinchpinException($"Symbol token '{token}' in '{path}' is not a valid integer.");
		}
	}

	[GeneratedRegex(@"^\.module\s+Module(?<module>\d+)$", RegexOptions.CultureInvariant)]
	private static partial Regex CasModuleRegex();

	[GeneratedRegex(@"^\.proc\s+(?<name>\S+)\s+locals\s+\d+$", RegexOptions.CultureInvariant)]
	private static partial Regex CasProcedureRegex();

	[GeneratedRegex(@"^;\s*original offsets:\s*start 0x(?<start>[0-9A-Fa-f]+),\s*code 0x[0-9A-Fa-f]+,\s*end 0x[0-9A-Fa-f]+$", RegexOptions.CultureInvariant)]
	private static partial Regex CasOffsetsRegex();
}