/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Text.Json;

namespace Linchpin.ToolTests;

public sealed record RuntimeCaseDefinition(
	string Name,
	string? MmePath,
	string? ObjPath,
	string? ChiselSourcePath,
	string? DataDirectoryPath,
	string InputText,
	int ExpectedInstructionCount,
	int? InstructionLimit,
	string ExpectedScreenText);

internal static class RuntimeCaseCatalog
{
	private static readonly Lazy<IReadOnlyList<RuntimeCaseDefinition>> CachedCases = new(LoadCases);

	public static IReadOnlyList<RuntimeCaseDefinition> AllCases => CachedCases.Value;

	public static IEnumerable<object[]> DynamicData()
	{
		foreach (RuntimeCaseDefinition testCase in AllCases)
		{
			yield return [testCase];
		}
	}

	private static IReadOnlyList<RuntimeCaseDefinition> LoadCases()
	{
		string manifestPath = Path.Combine(RepositoryLayout.Root, "test", "Linchpin.ToolTests", "TestData", "runtime-cases.json");
		RuntimeCaseManifest manifest = JsonSerializer.Deserialize<RuntimeCaseManifest>(File.ReadAllText(manifestPath), new JsonSerializerOptions
		{
			PropertyNameCaseInsensitive = true,
		}) ?? throw new InvalidOperationException($"Unable to parse runtime case manifest '{manifestPath}'.");

		return manifest.Cases.Select(CreateCase).ToArray();
	}

	private static RuntimeCaseDefinition CreateCase(RuntimeCaseManifestItem item)
	{
		string? mmePath = ResolveOptionalPath(item.MmePath);
		string? objPath = ResolveOptionalPath(item.ObjPath);
		string? chiselSourcePath = ResolveOptionalPath(item.ChiselSourcePath);
		if (chiselSourcePath is null && (mmePath is null || objPath is null))
		{
			throw new InvalidOperationException($"Runtime case '{item.Name}' must define either chiselSourcePath or both mmePath and objPath.");
		}

		string expectedScreenPath = ResolveRequiredPath(item.ExpectedScreenPath);
		return new RuntimeCaseDefinition(
			item.Name,
			mmePath,
			objPath,
			chiselSourcePath,
			ResolveOptionalPath(item.DataDirectoryPath),
			item.InputText ?? string.Empty,
			item.ExpectedInstructionCount,
			item.InstructionLimit,
			File.ReadAllText(expectedScreenPath).Replace("\r\n", "\n", StringComparison.Ordinal).TrimEnd('\n'));
	}

	private static string ResolveRequiredPath(string relativePath)
	{
		if (string.IsNullOrWhiteSpace(relativePath))
		{
			throw new InvalidOperationException("Runtime case manifest is missing a required path.");
		}

		return Path.GetFullPath(Path.Combine(RepositoryLayout.Root, relativePath));
	}

	private static string? ResolveOptionalPath(string? relativePath) =>
		string.IsNullOrWhiteSpace(relativePath)
			? null
			: Path.GetFullPath(Path.Combine(RepositoryLayout.Root, relativePath));

	private sealed class RuntimeCaseManifest
	{
		public List<RuntimeCaseManifestItem> Cases { get; set; } = [];
	}

	private sealed class RuntimeCaseManifestItem
	{
		public string Name { get; set; } = string.Empty;
		public string? MmePath { get; set; }
		public string? ObjPath { get; set; }
		public string? ChiselSourcePath { get; set; }
		public string? DataDirectoryPath { get; set; }
		public string InputText { get; set; } = string.Empty;
		public int ExpectedInstructionCount { get; set; }
		public int? InstructionLimit { get; set; }
		public string ExpectedScreenPath { get; set; } = string.Empty;
	}
}