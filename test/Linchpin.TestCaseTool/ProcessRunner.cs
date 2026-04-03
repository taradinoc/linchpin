/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Diagnostics;

namespace Linchpin.TestCaseTool;

internal static class ProcessRunner
{
    public static ProcessResult Run(string fileName, IReadOnlyList<string> arguments, string workingDirectory, IReadOnlyDictionary<string, string?>? environment = null)
    {
        using Process process = new();
        process.StartInfo = new ProcessStartInfo
        {
            FileName = fileName,
            WorkingDirectory = workingDirectory,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };

        foreach (string argument in arguments)
        {
            process.StartInfo.ArgumentList.Add(argument);
        }

        if (environment is not null)
        {
            foreach ((string key, string? value) in environment)
            {
                if (value is null)
                {
                    process.StartInfo.Environment.Remove(key);
                }
                else
                {
                    process.StartInfo.Environment[key] = value;
                }
            }
        }

        try
        {
            process.Start();
        }
        catch (Exception exception) when (exception is InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            throw new ToolException($"Failed to start process '{fileName}': {exception.Message}", exception);
        }

        string standardOutput = process.StandardOutput.ReadToEnd();
        string standardError = process.StandardError.ReadToEnd();
        if (!process.WaitForExit(120_000))
        {
            try
            {
                process.Kill(entireProcessTree: true);
            }
            catch (InvalidOperationException)
            {
            }

            throw new ToolException($"Process '{fileName}' did not exit within the allotted timeout.{Environment.NewLine}stdout:{Environment.NewLine}{standardOutput}{Environment.NewLine}stderr:{Environment.NewLine}{standardError}");
        }

        return new ProcessResult(process.ExitCode, standardOutput, standardError);
    }

    public static string ToWslPath(string path)
    {
        string fullPath = Path.GetFullPath(path);
        if (fullPath.Length < 3 || fullPath[1] != ':')
        {
            throw new ToolException($"Path '{fullPath}' is not a drive-qualified Windows path.");
        }

        char driveLetter = char.ToLowerInvariant(fullPath[0]);
        string remainder = fullPath[2..].Replace('\\', '/');
        return $"/mnt/{driveLetter}{remainder}";
    }

    public static string ShellQuote(string value) => $"'{value.Replace("'", "'\"'\"'", StringComparison.Ordinal)}'";
}
