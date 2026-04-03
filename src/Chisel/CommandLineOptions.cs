/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.CommandLine;

namespace Chisel;

internal static partial class Program
{
    /// <summary>Holds the resolved command-line options for a single Chisel assembler invocation.</summary>
	private sealed record CommandLineOptions(string InputPath, string ObjPath, string MmePath, string? GrammarPath)
	{
		/// <summary>Parses <paramref name="args"/> and invokes the appropriate assembler command.</summary>
		/// <param name="args">The command-line argument array from <c>Main</c>.</param>
		/// <returns>The process exit code: 0 on success, non-zero on failure.</returns>
		public static int Invoke(string[] args)
		{
			return CreateRootCommand().Parse(args).Invoke();
		}

		/// <summary>Builds the <see cref="System.CommandLine.RootCommand"/> with the assemble subcommand.</summary>
		/// <returns>The configured root command ready to be parsed and invoked.</returns>
		private static RootCommand CreateRootCommand()
		{
			ChiselCliOptions rootOptions = new();
			ChiselCliOptions assembleOptions = new();
			RootCommand root = new("Assemble Cornerstone CAS source into OBJ/MME images.")
			{
				TreatUnmatchedTokensAsErrors = true
			};
			rootOptions.AddTo(root);

			Command assembleCommand = new("assemble", "Assemble source into OBJ/MME output files.");
			assembleOptions.AddTo(assembleCommand);

			root.SetAction(parseResult => InvokeParsed(parseResult, rootOptions));
			assembleCommand.SetAction(parseResult => InvokeParsed(parseResult, assembleOptions));

			root.Subcommands.Add(assembleCommand);
			return root;
		}

		/// <summary>Extracts options from the parse result, constructs a <see cref="CommandLineOptions"/>, and runs the assembler.</summary>
		/// <param name="parseResult">The parse result from System.CommandLine.</param>
		/// <param name="cliOptions">The option/argument definitions to read values from.</param>
		/// <returns>The process exit code.</returns>
		private static int InvokeParsed(ParseResult parseResult, ChiselCliOptions cliOptions)
		{
			try
			{
				return Program.Assemble(FromParseResult(parseResult, cliOptions));
			}
			catch (AssemblerException exception)
			{
				Console.Error.WriteLine(exception.Message);
				return 1;
			}
		}

		/// <summary>Builds a <see cref="CommandLineOptions"/> from the parsed command-line values, applying defaults.</summary>
		/// <param name="parseResult">The parse result from System.CommandLine.</param>
		/// <param name="cliOptions">The option/argument definitions to read values from.</param>
		/// <returns>A fully populated <see cref="CommandLineOptions"/>.</returns>
		private static CommandLineOptions FromParseResult(ParseResult parseResult, ChiselCliOptions cliOptions)
		{
			string? inputPath = parseResult.GetValue(cliOptions.InputArgument);
			string? objPath = parseResult.GetValue(cliOptions.ObjOption);
			string? mmePath = parseResult.GetValue(cliOptions.MmeOption);
			string? grammarPath = parseResult.GetValue(cliOptions.GrammarOption);

			if (string.IsNullOrWhiteSpace(inputPath))
			{
				throw new AssemblerException("Input path must be supplied.");
			}

			if (string.IsNullOrWhiteSpace(mmePath))
			{
				throw new AssemblerException("--mme must be supplied.");
			}

			objPath ??= Path.ChangeExtension(mmePath, ".obj");
			if (string.IsNullOrWhiteSpace(objPath))
			{
				throw new AssemblerException("Could not determine an OBJ output path.");
			}
			return new CommandLineOptions(inputPath, objPath, mmePath, grammarPath);
		}

		/// <summary>Defines the arguments and options that all Chisel commands share.</summary>
		private sealed class ChiselCliOptions
		{
			/// <summary>The positional input source file argument.</summary>
			public Argument<string> InputArgument { get; } = new("input")
			{
				Description = "Input assembly source file.",
				HelpName = "input.cas"
			};

			/// <summary>The <c>--obj</c> option for specifying the output OBJ file path.</summary>
			public Option<string?> ObjOption { get; } = new("--obj")
			{
				Description = "Output OBJ path. Defaults to --mme with .obj extension."
			};

			/// <summary>The <c>--mme</c> option for specifying the output MME file path.</summary>
			public Option<string?> MmeOption { get; } = new("--mme")
			{
				Description = "Output MME path."
			};

			/// <summary>The <c>--grammar</c> option for specifying a custom grammar JSON file path.</summary>
			public Option<string?> GrammarOption { get; } = new("--grammar")
			{
				Description = "Path to instruction grammar JSON."
			};

			/// <summary>Registers all arguments and options onto the given command.</summary>
			/// <param name="command">The command to register onto.</param>
			public void AddTo(Command command)
			{
				command.Arguments.Add(InputArgument);
				command.Options.Add(ObjOption);
				command.Options.Add(MmeOption);
				command.Options.Add(GrammarOption);
			}
		}
	}
}
