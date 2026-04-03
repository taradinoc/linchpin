namespace Linchpin.LP128Pack;

internal static class Program
{
	public static int Main(string[] args)
	{
		try
		{
			PackOptions options = PackOptions.Parse(args);
			CornerstoneImage image = CornerstoneImageLoader.Load(options.MmePath, options.ObjPath);
			BundleWriter.Write(image, options.OutputPath);
			Console.WriteLine($"Wrote {options.OutputPath}");
			Console.WriteLine($"Entry selector: module {image.EntryPoint.ModuleId}, proc {image.EntryPoint.ProcedureIndex}");
			Console.WriteLine($"Modules: {image.Modules.Count}, exported procedures: {image.Modules.Sum(static module => module.Procedures.Count)}, initial RAM bytes: {image.InitialRamBytes.Length}, code bytes: {image.CodeEndOffset}");
			return 0;
		}
		catch (Exception ex) when (ex is LinchpinException or InvalidOperationException)
		{
			Console.Error.WriteLine(ex.Message);
			Console.Error.WriteLine("Usage: LP128Pack --mme <path> --obj <path> --output <path>");
			return 1;
		}
	}

	private sealed record PackOptions(string MmePath, string ObjPath, string OutputPath)
	{
		public static PackOptions Parse(string[] args)
		{
			string? mmePath = null;
			string? objPath = null;
			string? outputPath = null;

			for (int index = 0; index < args.Length; index++)
			{
				switch (args[index])
				{
					case "--mme":
						mmePath = RequireValue(args, ref index, "--mme");
						break;
					case "--obj":
						objPath = RequireValue(args, ref index, "--obj");
						break;
					case "--output":
						outputPath = RequireValue(args, ref index, "--output");
						break;
					default:
						throw new InvalidOperationException($"Unknown argument '{args[index]}'.");
				}
			}

			if (string.IsNullOrWhiteSpace(mmePath) || string.IsNullOrWhiteSpace(objPath) || string.IsNullOrWhiteSpace(outputPath))
			{
				throw new InvalidOperationException("All of --mme, --obj, and --output are required.");
			}

			return new PackOptions(Path.GetFullPath(mmePath), Path.GetFullPath(objPath), Path.GetFullPath(outputPath));
		}

		private static string RequireValue(string[] args, ref int index, string option)
		{
			if (index + 1 >= args.Length)
			{
				throw new InvalidOperationException($"Missing value for {option}.");
			}

			index++;
			return args[index];
		}
	}
}