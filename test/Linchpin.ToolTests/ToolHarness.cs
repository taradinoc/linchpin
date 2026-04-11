/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Diagnostics;
using System.Text;
using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Linchpin.ToolTests;

internal sealed record ProcessResult(int ExitCode, string StandardOutput, string StandardError);

internal sealed record ToolRuntimeReport(
	int HaltCode,
	int ExecutedInstructionCount,
	string ScreenText,
	bool? StoppedByInstructionLimit = null,
	string? StopReason = null,
	string? StopDetail = null,
	long? DefaultImageResolutionMicroseconds = null,
	long? ImageLoadMicroseconds = null,
	long? HostSetupMicroseconds = null,
	long? ExecInitMicroseconds = null,
	long? StartupMicroseconds = null,
	long? RunMicroseconds = null,
	long? TranscriptRenderMicroseconds = null,
	long? TotalMicroseconds = null);

internal sealed record ToolRunOutcome(ToolRuntimeReport Report, ProcessResult Process);

internal sealed record RuntimeImagePaths(string MmePath, string ObjPath);

internal static class ToolHarness
{
	private static readonly JsonSerializerOptions JsonOptions = new()
	{
		PropertyNameCaseInsensitive = true,
	};

	private static readonly Lazy<(bool Available, string Message)> LinchpinStAvailability = new(ProbeLinchpinStAvailability);

	public static bool IsLinchpinStAvailable => LinchpinStAvailability.Value.Available;

	public static void RequireLinchpinSt()
	{
		(bool available, string message) = LinchpinStAvailability.Value;
		if (!available)
		{
			Assert.Inconclusive(message);
		}
	}

	public static ToolRunOutcome RunLinchpin(RuntimeCaseDefinition testCase, IReadOnlyDictionary<string, string?>? environmentOverrides = null)
	{
		using RuntimeWorkspace workspace = RuntimeWorkspace.Create(testCase.Name, testCase.DataDirectoryPath, testCase.MmePath);
		RuntimeImagePaths imagePaths = ResolveRuntimeImagePaths(testCase, workspace);
		return RunLinchpin(testCase, workspace, imagePaths.MmePath, imagePaths.ObjPath, environmentOverrides);
	}

	public static ToolRunOutcome RunLinchpin(
		RuntimeCaseDefinition testCase,
		RuntimeWorkspace workspace,
		string mmePath,
		string objPath,
		IReadOnlyDictionary<string, string?>? environmentOverrides = null)
	{
		string inputPath = workspace.WriteInputFile(testCase.InputText);
		string reportPath = Path.Combine(workspace.RootPath, "linchpin-report.json");
		Dictionary<string, string?> environment = new(workspace.BuildEnvironment(), StringComparer.Ordinal);
		if (testCase.InstructionLimit.HasValue)
		{
			environment["LINCHPIN_EXECUTION_LIMIT"] = testCase.InstructionLimit.Value.ToString();
		}

		if (environmentOverrides is not null)
		{
			foreach ((string key, string? value) in environmentOverrides)
			{
				environment[key] = value;
			}
		}

		ProcessResult process = RunProcess(
			"dotnet",
			[
				"run",
				"--no-build",
				"--project",
				Path.Combine(RepositoryLayout.Root, "src", "Linchpin"),
				"--",
				"run",
				"--mme",
				mmePath,
				"--obj",
				objPath,
				"--input-file",
				inputPath,
				"--test-report",
				reportPath,
			],
			RepositoryLayout.Root,
			environment);

		return new ToolRunOutcome(LoadReport(reportPath), process);
	}

	public static ProcessResult RunLinchpinInspectShipped()
	{
		return RunProcess(
			"dotnet",
			[
				"run",
				"--no-build",
				"--project",
				Path.Combine(RepositoryLayout.Root, "src", "Linchpin"),
				"--",
				"inspect",
				"--shipped",
			],
			RepositoryLayout.Root);
	}

	public static ProcessResult RunLinchpinDisassembleShipped(int moduleId, string jsonOutputPath)
	{
		return RunProcess(
			"dotnet",
			[
				"run",
				"--no-build",
				"--project",
				Path.Combine(RepositoryLayout.Root, "src", "Linchpin"),
				"--",
				"disassemble",
				"--shipped",
				"--module",
				moduleId.ToString(),
				"--json-output",
				jsonOutputPath,
			],
			RepositoryLayout.Root);
	}

	public static ToolRunOutcome RunLinchpinSt(RuntimeCaseDefinition testCase)
	{
		RequireLinchpinSt();
		using RuntimeWorkspace workspace = RuntimeWorkspace.Create(testCase.Name, testCase.DataDirectoryPath, testCase.MmePath);
		RuntimeImagePaths imagePaths = ResolveRuntimeImagePaths(testCase, workspace);
		string mmePath = imagePaths.MmePath;
		string inputPath = workspace.WriteInputFile(testCase.InputText);
		string reportPath = Path.Combine(workspace.RootPath, "linchpinst-report.json");
		string toolDirectory = ToWslPath(Path.Combine(RepositoryLayout.Root, "src", "LinchpinST"));
		string command = $"cd {ShellQuote(toolDirectory)} && ";
		if (!string.IsNullOrWhiteSpace(workspace.SampleDirectoryPath))
		{
			command += $"LINCHPIN_DATA_DIR={ShellQuote(ToWslPath(workspace.SampleDirectoryPath!))} ";
		}

		command += $"./bin/linchpin_st run {ShellQuote(ToWslPath(mmePath))} --input-file {ShellQuote(ToWslPath(inputPath))} --test-report {ShellQuote(ToWslPath(reportPath))} --transcript";
		if (testCase.InstructionLimit.HasValue)
		{
			command += $" --limit {testCase.InstructionLimit.Value}";
		}

		ProcessResult process = RunProcess("wsl", ["sh", "-lc", command], RepositoryLayout.Root);
		return new ToolRunOutcome(LoadReport(reportPath), process);
	}

	public static ProcessResult RunChiselAssemble(string sourcePath, string mmePath, string objPath)
	{
		return RunProcess(
			"dotnet",
			[
				"run",
				"--no-build",
				"--project",
				Path.Combine(RepositoryLayout.Root, "src", "Chisel"),
				"--",
				"assemble",
				sourcePath,
				"--obj",
				objPath,
				"--mme",
				mmePath,
			],
			RepositoryLayout.Root);
	}

	public static string DescribeProcessFailure(string toolName, ProcessResult process) =>
		$"{toolName} exited with code {process.ExitCode}.{Environment.NewLine}stdout:{Environment.NewLine}{process.StandardOutput}{Environment.NewLine}stderr:{Environment.NewLine}{process.StandardError}";

	public static void AssertEquivalentScreenText(string expected, string actual, string failureMessage)
	{
		Assert.AreEqual(NormalizeScreenTextForComparison(expected), NormalizeScreenTextForComparison(actual), failureMessage);
	}

	public static string NormalizeScreenTextForComparison(string text) =>
		string.IsNullOrEmpty(text)
			? string.Empty
			: text.Replace('─', '-');

	private static RuntimeImagePaths ResolveRuntimeImagePaths(RuntimeCaseDefinition testCase, RuntimeWorkspace workspace)
	{
		if (!string.IsNullOrWhiteSpace(testCase.ChiselSourcePath))
		{
			string mmePath = Path.Combine(workspace.RootPath, $"{testCase.Name}.mme");
			string objPath = Path.Combine(workspace.RootPath, $"{testCase.Name}.obj");
			ProcessResult assemble = RunChiselAssemble(testCase.ChiselSourcePath!, mmePath, objPath);
			Assert.AreEqual(0, assemble.ExitCode, DescribeProcessFailure("Chisel", assemble));
			return new RuntimeImagePaths(mmePath, objPath);
		}

		if (string.IsNullOrWhiteSpace(testCase.MmePath) || string.IsNullOrWhiteSpace(testCase.ObjPath))
		{
			throw new InvalidOperationException($"Runtime case '{testCase.Name}' does not define runnable image paths.");
		}

		string stagedMmePath = workspace.CopyFile(testCase.MmePath!);
		string stagedObjPath = workspace.CopyFile(testCase.ObjPath!);
		return new RuntimeImagePaths(stagedMmePath, stagedObjPath);
	}

	private static ToolRuntimeReport LoadReport(string reportPath)
	{
		if (!File.Exists(reportPath))
		{
			throw new InvalidOperationException($"Expected test report '{reportPath}' was not written.");
		}

		ToolRuntimeReport? report = JsonSerializer.Deserialize<ToolRuntimeReport>(File.ReadAllText(reportPath), JsonOptions);
		return report ?? throw new InvalidOperationException($"Unable to parse test report '{reportPath}'.");
	}

	private static ProcessResult RunProcess(string fileName, IReadOnlyList<string> arguments, string workingDirectory, IReadOnlyDictionary<string, string?>? environment = null)
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
			throw new InvalidOperationException($"Failed to start process '{fileName}': {exception.Message}", exception);
		}

		Task<string> standardOutputTask = process.StandardOutput.ReadToEndAsync();
		Task<string> standardErrorTask = process.StandardError.ReadToEndAsync();
		if (!process.WaitForExit(120_000))
		{
			try
			{
				process.Kill(entireProcessTree: true);
			}
			catch (InvalidOperationException)
			{
			}

			Task.WaitAll([standardOutputTask, standardErrorTask], 5_000);
			string timedOutStandardOutput = standardOutputTask.IsCompleted ? standardOutputTask.Result : string.Empty;
			string timedOutStandardError = standardErrorTask.IsCompleted ? standardErrorTask.Result : string.Empty;

			throw new TimeoutException($"Process '{fileName}' did not exit within the allotted timeout.{Environment.NewLine}stdout:{Environment.NewLine}{timedOutStandardOutput}{Environment.NewLine}stderr:{Environment.NewLine}{timedOutStandardError}");
		}

		Task.WaitAll([standardOutputTask, standardErrorTask]);
		string standardOutput = standardOutputTask.Result;
		string standardError = standardErrorTask.Result;

		return new ProcessResult(process.ExitCode, standardOutput, standardError);
	}

	private static (bool Available, string Message) ProbeLinchpinStAvailability()
	{
		try
		{
			string toolDirectory = ToWslPath(Path.Combine(RepositoryLayout.Root, "src", "LinchpinST"));
			ProcessResult process = RunProcess("wsl", ["sh", "-lc", $"cd {ShellQuote(toolDirectory)} && make posix"], RepositoryLayout.Root);
			return process.ExitCode == 0
				? (true, string.Empty)
				: (false, $"LinchpinST POSIX build failed.{Environment.NewLine}{DescribeProcessFailure("wsl make posix", process)}");
		}
		catch (Exception exception)
		{
			return (false, $"LinchpinST POSIX prerequisites are unavailable: {exception.Message}");
		}
	}

	private static string ToWslPath(string path)
	{
		string fullPath = Path.GetFullPath(path);
		if (fullPath.Length < 3 || fullPath[1] != ':')
		{
			throw new InvalidOperationException($"Path '{fullPath}' is not a drive-qualified Windows path.");
		}

		char driveLetter = char.ToLowerInvariant(fullPath[0]);
		string remainder = fullPath[2..].Replace('\\', '/');
		return $"/mnt/{driveLetter}{remainder}";
	}

	private static string ShellQuote(string value) => $"'{value.Replace("'", "'\"'\"'", StringComparison.Ordinal)}'";
}

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
		string rootPath = Path.Combine(Path.GetTempPath(), "cornerstone-tooltests", $"{Sanitize(caseName)}-{Guid.NewGuid():N}");
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

	public string CopyFile(string sourcePath)
	{
		string destinationPath = Path.Combine(RootPath, Path.GetFileName(sourcePath));
		File.Copy(sourcePath, destinationPath, overwrite: true);
		return destinationPath;
	}

	public IReadOnlyDictionary<string, string?> BuildEnvironment()
	{
		Dictionary<string, string?> environment = new(StringComparer.Ordinal)
		{
			["LINCHPIN_DATA_DIR"] = SampleDirectoryPath,
		};

		return environment;
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