/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Text.Json;

namespace Linchpin.TestCaseTool;

internal sealed class InterpreterRunner
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
    };

    private readonly string repositoryRoot;
    private readonly InterpreterKind kind;

    private InterpreterRunner(InterpreterKind kind, string repositoryRoot, string displayName)
    {
        this.kind = kind;
        this.repositoryRoot = repositoryRoot;
        DisplayName = displayName;
    }

    public string DisplayName { get; }

    public static InterpreterRunner Create(InterpreterKind kind, string repositoryRoot) => kind switch
    {
        InterpreterKind.Linchpin => new(kind, repositoryRoot, "Linchpin"),
        InterpreterKind.LinchpinSt => new(kind, repositoryRoot, "LinchpinST"),
        _ => throw new ToolException($"Unsupported interpreter kind '{kind}'."),
    };

    public ToolRunOutcome Run(RuntimeExecutionRequest request)
    {
        using RuntimeWorkspace workspace = RuntimeWorkspace.Create(request.Name, request.DataDirectoryPath, request.MmePath);
        RuntimeImagePaths imagePaths = ResolveRuntimeImagePaths(request, workspace);
        string mmePath = imagePaths.MmePath;
        string objPath = imagePaths.ObjPath;
        string inputPath = workspace.WriteInputFile(request.InputText);
        string reportPath = Path.Combine(workspace.RootPath, $"{DisplayName.ToLowerInvariant()}-report.json");

        return kind switch
        {
            InterpreterKind.Linchpin => RunLinchpin(workspace, mmePath, objPath, inputPath, reportPath, request.InstructionLimit),
            InterpreterKind.LinchpinSt => RunLinchpinSt(workspace, mmePath, inputPath, reportPath, request.InstructionLimit),
            _ => throw new ToolException($"Unsupported interpreter kind '{kind}'."),
        };
    }

    private RuntimeImagePaths ResolveRuntimeImagePaths(RuntimeExecutionRequest request, RuntimeWorkspace workspace)
    {
        if (!string.IsNullOrWhiteSpace(request.SourcePath))
        {
            string mmePath = Path.Combine(workspace.RootPath, $"{request.Name}.mme");
            string objPath = Path.Combine(workspace.RootPath, $"{request.Name}.obj");
            ProcessResult assemble = ProcessRunner.Run(
                "dotnet",
                [
                    "run",
                    "--project",
                    Path.Combine(repositoryRoot, "tools", "Chisel"),
                    "--",
                    "assemble",
                    request.SourcePath,
                    "--obj",
                    objPath,
                    "--mme",
                    mmePath,
                ],
                repositoryRoot);
            if (assemble.ExitCode != 0)
            {
                throw new ToolException($"Chisel failed while assembling '{request.SourcePath}'.{Environment.NewLine}stdout:{Environment.NewLine}{assemble.StandardOutput}{Environment.NewLine}stderr:{Environment.NewLine}{assemble.StandardError}");
            }

            return new RuntimeImagePaths(mmePath, objPath);
        }

        if (string.IsNullOrWhiteSpace(request.MmePath) || string.IsNullOrWhiteSpace(request.ObjPath))
        {
            throw new ToolException("A runtime execution request must define either sourcePath or both mmePath and objPath.");
        }

        return new RuntimeImagePaths(workspace.CopyFile(request.MmePath!), workspace.CopyFile(request.ObjPath!));
    }

    private ToolRunOutcome RunLinchpin(RuntimeWorkspace workspace, string mmePath, string objPath, string inputPath, string reportPath, int? instructionLimit)
    {
        Dictionary<string, string?> environment = new(workspace.BuildEnvironment(), StringComparer.Ordinal);
        if (instructionLimit.HasValue)
        {
            environment["LINCHPIN_EXECUTION_LIMIT"] = instructionLimit.Value.ToString();
        }

        ProcessResult process = ProcessRunner.Run(
            "dotnet",
            [
                "run",
                "--project",
                Path.Combine(repositoryRoot, "tools", "Linchpin"),
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
            repositoryRoot,
            environment);

        return new ToolRunOutcome(LoadReport(reportPath), process);
    }

    private ToolRunOutcome RunLinchpinSt(RuntimeWorkspace workspace, string mmePath, string inputPath, string reportPath, int? instructionLimit)
    {
        EnsureLinchpinStAvailable();
        string toolDirectory = ProcessRunner.ToWslPath(Path.Combine(repositoryRoot, "tools", "LinchpinST"));
        string command = $"cd {ProcessRunner.ShellQuote(toolDirectory)} && ";
        if (!string.IsNullOrWhiteSpace(workspace.SampleDirectoryPath))
        {
            command += $"LINCHPIN_DATA_DIR={ProcessRunner.ShellQuote(ProcessRunner.ToWslPath(workspace.SampleDirectoryPath!))} ";
        }

        command += $"./bin/linchpin_st run {ProcessRunner.ShellQuote(ProcessRunner.ToWslPath(mmePath))} --input-file {ProcessRunner.ShellQuote(ProcessRunner.ToWslPath(inputPath))} --test-report {ProcessRunner.ShellQuote(ProcessRunner.ToWslPath(reportPath))} --transcript";
        if (instructionLimit.HasValue)
        {
            command += $" --limit {instructionLimit.Value}";
        }

        ProcessResult process = ProcessRunner.Run("wsl", ["sh", "-lc", command], repositoryRoot);
        return new ToolRunOutcome(LoadReport(reportPath), process);
    }

    private static ToolRuntimeReport LoadReport(string reportPath)
    {
        if (!File.Exists(reportPath))
        {
            throw new ToolException($"Expected test report '{reportPath}' was not written.");
        }

        ToolRuntimeReport? report = JsonSerializer.Deserialize<ToolRuntimeReport>(File.ReadAllText(reportPath), JsonOptions);
        return report ?? throw new ToolException($"Unable to parse test report '{reportPath}'.");
    }

    private void EnsureLinchpinStAvailable()
    {
        ProcessResult process;
        try
        {
            string toolDirectory = ProcessRunner.ToWslPath(Path.Combine(repositoryRoot, "tools", "LinchpinST"));
            process = ProcessRunner.Run("wsl", ["sh", "-lc", $"cd {ProcessRunner.ShellQuote(toolDirectory)} && make posix"], repositoryRoot);
        }
        catch (Exception exception)
        {
            throw new ToolException($"Unable to prepare LinchpinST: {exception.Message}");
        }

        if (process.ExitCode != 0)
        {
            throw new ToolException($"LinchpinST POSIX build failed.{Environment.NewLine}{process.StandardOutput}{Environment.NewLine}{process.StandardError}");
        }
    }
}
