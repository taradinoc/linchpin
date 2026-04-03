/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.CommandLine;

namespace Linchpin;

/// <summary>
/// Parsed command-line options for Linchpin. Immutable record produced by <see cref="FromParseResult"/>.
/// </summary>
internal sealed record CommandLineOptions(
    string MmePath,
    string ObjPath,
    string? GrammarPath,
    LinchpinCommand Command,
    bool EmitSourceIncludeStats,
    bool EmitSourceIncludeOffsets,
    bool EmitSourceIncludePadding,
    string InputText,
    bool HasExplicitInputText,
    bool HostWriteFiles,
    string? DataPath,
    string? OutputPath,
    string? JsonOutputPath,
    string? TestReportPath,
    int? ModuleFilter,
    int? ProcedureFilter)
{
    /// <summary>
    /// Parses and invokes the command line, returning an exit code.
    /// </summary>
    /// <param name="args">Raw command-line arguments.</param>
    /// <returns>Exit code.</returns>
    public static int Invoke(string[] args)
    {
        return CreateRootCommand().Parse(args).Invoke();
    }

    /// <summary>
    /// Builds the System.CommandLine root command with all subcommands and options.
    /// </summary>
    private static RootCommand CreateRootCommand()
    {
        LinchpinCliOptions rootOptions = new();
        LinchpinCliOptions runOptions = new();
        LinchpinCliOptions inspectOptions = new();
        LinchpinCliOptions disassembleOptions = new();

        RootCommand root = new("An interpreter and disassembler for the Cornerstone VM.")
        {
            TreatUnmatchedTokensAsErrors = true
        };
        rootOptions.AddTo(root, includeEmitSourceOptions: false);

        Command runCommand = new("run", "Run the VM.");
        runOptions.AddTo(runCommand, includeEmitSourceOptions: false);

        Command inspectCommand = new("inspect", "Inspect image metadata and entrypoint preview.");
        inspectOptions.AddTo(inspectCommand, includeEmitSourceOptions: false);

        Command disassembleCommand = new("disassemble", "Emit disassembled source.");
        disassembleOptions.AddTo(disassembleCommand, includeEmitSourceOptions: true);

        root.SetAction(parseResult => InvokeParsed(parseResult, LinchpinCommand.Run, rootOptions));
        runCommand.SetAction(parseResult => InvokeParsed(parseResult, LinchpinCommand.Run, runOptions));
        inspectCommand.SetAction(parseResult => InvokeParsed(parseResult, LinchpinCommand.Inspect, inspectOptions));
        disassembleCommand.SetAction(parseResult => InvokeParsed(parseResult, LinchpinCommand.EmitSource, disassembleOptions));

        root.Subcommands.Add(runCommand);
        root.Subcommands.Add(inspectCommand);
        root.Subcommands.Add(disassembleCommand);
        return root;
    }

    /// <summary>
    /// Handles a parsed command invocation, converting CLI tokens into a <see cref="CommandLineOptions"/> record.
    /// </summary>
    private static int InvokeParsed(ParseResult parseResult, LinchpinCommand command, LinchpinCliOptions cliOptions)
    {
        try
        {
            return Program.Run(FromParseResult(parseResult, command, cliOptions));
        }
        catch (LinchpinException exception)
        {
            Console.Error.WriteLine(exception.Message);
            return 1;
        }
    }

    private static CommandLineOptions FromParseResult(ParseResult parseResult, LinchpinCommand command, LinchpinCliOptions cliOptions)
    {
        bool useShipped = parseResult.GetValue(cliOptions.ShippedOption);
        string? mmePath = parseResult.GetValue(cliOptions.MmeOption);
        string? objPath = parseResult.GetValue(cliOptions.ObjOption);
        string? grammarPath = parseResult.GetValue(cliOptions.GrammarOption);
        string? inputText = parseResult.GetValue(cliOptions.InputTextOption);
        string? inputFile = parseResult.GetValue(cliOptions.InputFileOption);
        string? dataPath = parseResult.GetValue(cliOptions.DataOption);
        string? outputPath = parseResult.GetValue(cliOptions.OutputOption);
        bool hostWriteFiles = parseResult.GetValue(cliOptions.HostWriteFilesOption);
        string? jsonOutputPath = parseResult.GetValue(cliOptions.JsonOutputOption);
        string? testReportPath = parseResult.GetValue(cliOptions.TestReportOption);
        string? moduleToken = parseResult.GetValue(cliOptions.ModuleOption);
        string? procedureToken = parseResult.GetValue(cliOptions.ProcedureOption);
        bool emitSourceIncludeStats = command == LinchpinCommand.EmitSource && parseResult.GetValue(cliOptions.StatsOption);
        bool emitSourceIncludeOffsets = command == LinchpinCommand.EmitSource && parseResult.GetValue(cliOptions.OffsetsOption);
        bool emitSourceIncludePadding = command == LinchpinCommand.EmitSource && parseResult.GetValue(cliOptions.PaddingOption);

        bool hasInputText = parseResult.GetResult(cliOptions.InputTextOption) is not null;
        bool hasInputFile = parseResult.GetResult(cliOptions.InputFileOption) is not null;
        if (hasInputText && hasInputFile)
        {
            throw new LinchpinException("Use either --input-text or --input-file, not both.");
        }

        if (hasInputFile)
        {
            inputText = ReadInputTextFromFile(inputFile!);
        }

        int? moduleFilter = moduleToken is null
            ? null
            : ParseNonNegativeInteger(moduleToken, "module");
        int? procedureFilter = procedureToken is null
            ? null
            : ParseNonNegativeInteger(procedureToken, "procedure");

        if (procedureFilter.HasValue && !moduleFilter.HasValue)
        {
            throw new LinchpinException("--procedure requires --module.");
        }

        if (useShipped && (mmePath is not null || objPath is not null))
        {
            throw new LinchpinException("Use either --shipped or explicit --mme/--obj paths, not both.");
        }

        if (useShipped || string.IsNullOrWhiteSpace(mmePath))
        {
            return ResolveShipped(
                grammarPath,
                command,
                inputText ?? DefaultInputText(command),
                hasInputText || hasInputFile,
                hostWriteFiles,
                dataPath,
                outputPath,
                jsonOutputPath,
                testReportPath,
                moduleFilter,
                procedureFilter,
                emitSourceIncludeStats,
                emitSourceIncludeOffsets,
                emitSourceIncludePadding);
        }

        objPath ??= Path.ChangeExtension(mmePath, ".obj");

        return new CommandLineOptions(
            Path.GetFullPath(mmePath),
            Path.GetFullPath(objPath),
            grammarPath is null ? null : Path.GetFullPath(grammarPath),
            command,
            emitSourceIncludeStats,
            emitSourceIncludeOffsets,
            emitSourceIncludePadding,
            inputText ?? DefaultInputText(command),
            hasInputText || hasInputFile,
            hostWriteFiles,
            dataPath is null ? null : Path.GetFullPath(dataPath),
            outputPath is null ? null : Path.GetFullPath(outputPath),
            jsonOutputPath is null ? null : Path.GetFullPath(jsonOutputPath),
            testReportPath is null ? null : Path.GetFullPath(testReportPath),
            moduleFilter,
            procedureFilter);
    }

    private sealed class LinchpinCliOptions
    {
        public Option<bool> ShippedOption { get; } = new("--shipped") { Description = "Use shipped CORNER.MME/CORNER.OBJ paths." };
        public Option<string?> MmeOption { get; } = new("--mme") { Description = "Path to the .MME metadata file." };
        public Option<string?> ObjOption { get; } = new("--obj") { Description = "Path to the .OBJ bytecode file." };
        public Option<string?> GrammarOption { get; } = new("--grammar") { Description = "Path to opcode grammar JSON." };
        public Option<string?> InputTextOption { get; } = new("--input-text") { Description = "Latin-1 input text supplied to run mode." };
        public Option<string?> InputFileOption { get; } = new("--input-file") { Description = "Path to a Latin-1 input text file for run mode." };
        public Option<string?> DataOption { get; } = new("--data") { Description = "Host data directory for VM file I/O." };
        public Option<string?> OutputOption { get; } = new("--output") { Description = "Output file path." };
        public Option<bool> HostWriteFilesOption { get; } = new("--host-write-files") { Description = "Allow host file writes when running." };
        public Option<string?> JsonOutputOption { get; } = new("--json-output") { Description = "JSON disassembly output path." };
        public Option<string?> TestReportOption { get; } = new("--test-report") { Description = "VM test report output path." };
        public Option<string?> ModuleOption { get; } = new("--module") { Description = "Module id (decimal or hex with 0x prefix)." };
        public Option<string?> ProcedureOption { get; } = new("--procedure") { Description = "Procedure index (decimal or hex with 0x prefix). Requires --module." };
        public Option<bool> StatsOption { get; } = new("--stats") { Description = "Include summary statistics in disassembly output." };
        public Option<bool> OffsetsOption { get; } = new("--offsets") { Description = "Include byte offsets in disassembly output." };
        public Option<bool> PaddingOption { get; } = new("--padding") { Description = "Include decoded padding directives in disassembly output." };

        public void AddTo(Command command, bool includeEmitSourceOptions)
        {
            foreach (Option option in new Option[]
                {
                    ShippedOption,
                    MmeOption,
                    ObjOption,
                    GrammarOption,
                    InputTextOption,
                    InputFileOption,
                    DataOption,
                    OutputOption,
                    HostWriteFilesOption,
                    JsonOutputOption,
                    TestReportOption,
                    ModuleOption,
                    ProcedureOption,
                })
            {
                command.Options.Add(option);
            }

            if (includeEmitSourceOptions)
            {
                command.Options.Add(StatsOption);
                command.Options.Add(OffsetsOption);
                command.Options.Add(PaddingOption);
            }
        }
    }

    /// <summary>
    /// Resolves the Cornerstone image paths when <c>--shipped</c> is used, locating the repository's
    /// bundled CORNER.MME and CORNER.OBJ files.
    /// </summary>
    private static CommandLineOptions ResolveShipped(
        string? grammarPath,
        LinchpinCommand command,
        string inputText,
        bool hasExplicitInputText,
        bool hostWriteFiles,
        string? dataPath,
        string? outputPath,
        string? jsonOutputPath,
        string? testReportPath,
        int? moduleFilter,
        int? procedureFilter,
        bool emitSourceIncludeStats,
        bool emitSourceIncludeOffsets,
        bool emitSourceIncludePadding)
    {
        string repoRoot = RepositoryLocator.FindRepositoryRoot();
        return new CommandLineOptions(
            Path.Combine(repoRoot, "Cornerstone", "CORNER.MME"),
            Path.Combine(repoRoot, "Cornerstone", "CORNER.OBJ"),
            grammarPath is null ? null : Path.GetFullPath(grammarPath),
            command,
            emitSourceIncludeStats,
            emitSourceIncludeOffsets,
            emitSourceIncludePadding,
            inputText,
            hasExplicitInputText,
            hostWriteFiles,
            dataPath is null ? null : Path.GetFullPath(dataPath),
            outputPath is null ? null : Path.GetFullPath(outputPath),
            jsonOutputPath is null ? null : Path.GetFullPath(jsonOutputPath),
            testReportPath is null ? null : Path.GetFullPath(testReportPath),
            moduleFilter,
            procedureFilter);
    }

    private static string ReadInputTextFromFile(string path)
    {
        string fullPath = Path.GetFullPath(path);
        if (!File.Exists(fullPath))
        {
            throw new LinchpinException($"Input file '{fullPath}' does not exist.");
        }

        return System.Text.Encoding.Latin1.GetString(File.ReadAllBytes(fullPath));
    }

    private static string DefaultInputText(LinchpinCommand command) => command == LinchpinCommand.Run ? "x" : string.Empty;

    private static int ParseNonNegativeInteger(string token, string description)
    {
        try
        {
            return token.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
                ? Convert.ToInt32(token, 16)
                : int.Parse(token);
        }
        catch (FormatException)
        {
            throw new LinchpinException($"Invalid {description} value '{token}'.");
        }
        catch (OverflowException)
        {
            throw new LinchpinException($"Invalid {description} value '{token}'.");
        }
    }
}
