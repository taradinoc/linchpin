/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Text.Json;

namespace Linchpin.TestCaseTool;

internal sealed class ManifestStore
{
    private readonly string repositoryRoot;
    private readonly string manifestPath;
    private readonly string runtimeDirectoryPath;

    public ManifestStore(string repositoryRoot)
    {
        this.repositoryRoot = repositoryRoot;
        manifestPath = Path.Combine(repositoryRoot, "tests", "Cornerstone.ToolTests", "TestData", "runtime-cases.json");
        runtimeDirectoryPath = Path.Combine(repositoryRoot, "tests", "Cornerstone.ToolTests", "TestData", "runtime");
    }

    public ManifestCase? TryGetCase(string name) => LoadManifest().Cases.FirstOrDefault(testCase => testCase.Name.Equals(name, StringComparison.OrdinalIgnoreCase));

    public string WriteScreenFixture(string caseName, string screenText)
    {
        Directory.CreateDirectory(runtimeDirectoryPath);
        string relativePath = $"tests/Cornerstone.ToolTests/TestData/runtime/{caseName}.screen.txt";
        string fullPath = Path.Combine(repositoryRoot, relativePath.Replace('/', Path.DirectorySeparatorChar));
        File.WriteAllText(fullPath, screenText + Environment.NewLine);
        return relativePath;
    }

    public ManifestCase UpsertCase(ManifestCase value)
    {
        ManifestFile manifest = LoadManifest();
        ManifestCase? existing = manifest.Cases.FirstOrDefault(testCase => testCase.Name.Equals(value.Name, StringComparison.OrdinalIgnoreCase));
        if (existing is not null)
        {
            manifest.Cases.Remove(existing);
        }

        manifest.Cases.Add(value);
        manifest.Cases = manifest.Cases.OrderBy(testCase => testCase.Name, StringComparer.OrdinalIgnoreCase).ToList();
        SaveManifest(manifest);
        return value;
    }

    private ManifestFile LoadManifest()
    {
        if (!File.Exists(manifestPath))
        {
            return new ManifestFile();
        }

        ManifestFile? manifest = JsonSerializer.Deserialize<ManifestFile>(File.ReadAllText(manifestPath), new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true,
        });
        return manifest ?? new ManifestFile();
    }

    private void SaveManifest(ManifestFile manifest)
    {
        string json = JsonSerializer.Serialize(manifest, ProgramJson.Options);
        File.WriteAllText(manifestPath, json + Environment.NewLine);
    }

    private sealed class ManifestFile
    {
        public List<ManifestCase> Cases { get; set; } = [];
    }
}
