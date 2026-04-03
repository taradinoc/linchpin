/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Text;

namespace Linchpin.TestCaseTool;

internal sealed class RuntimeWorkspace : IDisposable
{
    private RuntimeWorkspace(string rootPath, string? sampleDirectoryPath)
    {
        RootPath = rootPath;
        SampleDirectoryPath = sampleDirectoryPath;
    }

    public string RootPath { get; }

    public string? SampleDirectoryPath { get; }

    public static RuntimeWorkspace Create(string caseName, string? sampleDirectorySource, string? mmePath = null)
    {
        string rootPath = Path.Combine(Path.GetTempPath(), "cornerstone-case-tool", $"{Sanitize(caseName)}-{Guid.NewGuid():N}");
        Directory.CreateDirectory(rootPath);

        string sampleDirectoryPath = Path.Combine(rootPath, "Sample");
        bool hasExplicitSampleDirectory = !string.IsNullOrWhiteSpace(sampleDirectorySource);
        bool hasSampleDirectory = false;
        if (hasExplicitSampleDirectory)
        {
            CopyDirectory(sampleDirectorySource!, sampleDirectoryPath);
            hasSampleDirectory = true;
        }

        if (!hasExplicitSampleDirectory)
        {
            string? companionSampleDirectory = ResolveCompanionSampleDirectory(mmePath);
            if (!string.IsNullOrWhiteSpace(companionSampleDirectory))
            {
                CopyDirectory(companionSampleDirectory!, sampleDirectoryPath);
                hasSampleDirectory = true;
            }
        }

        return new RuntimeWorkspace(rootPath, hasSampleDirectory ? sampleDirectoryPath : null);
    }

    public IReadOnlyDictionary<string, string?> BuildEnvironment()
    {
        Dictionary<string, string?> environment = new(StringComparer.Ordinal)
        {
            ["LINCHPIN_DATA_DIR"] = SampleDirectoryPath,
        };

        return environment;
    }

    public string CopyFile(string sourcePath)
    {
        string destinationPath = Path.Combine(RootPath, Path.GetFileName(sourcePath));
        File.Copy(sourcePath, destinationPath, overwrite: true);
        return destinationPath;
    }

    public string WriteInputFile(string inputText)
    {
        string inputPath = Path.Combine(RootPath, "input.bin");
        File.WriteAllBytes(inputPath, Encoding.Latin1.GetBytes(inputText));
        return inputPath;
    }

    public void Dispose()
    {
        if (Directory.Exists(RootPath))
        {
            Directory.Delete(RootPath, recursive: true);
        }
    }

    private static void CopyDirectory(string sourceDirectory, string destinationDirectory)
    {
        Directory.CreateDirectory(destinationDirectory);
        foreach (string file in Directory.EnumerateFiles(sourceDirectory))
        {
            File.Copy(file, Path.Combine(destinationDirectory, Path.GetFileName(file)), overwrite: true);
        }

        foreach (string directory in Directory.EnumerateDirectories(sourceDirectory))
        {
            CopyDirectory(directory, Path.Combine(destinationDirectory, Path.GetFileName(directory)));
        }
    }

    private static string? ResolveCompanionSampleDirectory(string? mmePath)
    {
        if (string.IsNullOrWhiteSpace(mmePath))
        {
            return null;
        }

        string? imageDirectory = Path.GetDirectoryName(Path.GetFullPath(mmePath));
        if (string.IsNullOrWhiteSpace(imageDirectory))
        {
            return null;
        }

        DirectoryInfo? parentDirectory = Directory.GetParent(imageDirectory);
        if (parentDirectory is null)
        {
            return null;
        }

        foreach (string candidateName in new[] { "Sample", "SAMPLE" })
        {
            string candidatePath = Path.Combine(parentDirectory.FullName, candidateName);
            if (Directory.Exists(candidatePath))
            {
                return candidatePath;
            }
        }

        return null;
    }

    private static string Sanitize(string name)
    {
        StringBuilder builder = new(name.Length);
        foreach (char character in name)
        {
            builder.Append(char.IsLetterOrDigit(character) ? character : '-');
        }

        return builder.ToString().Trim('-');
    }
}
