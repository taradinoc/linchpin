/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Linchpin.ToolTests;

internal static class RepositoryLayout
{
	private static readonly Lazy<string> RootPath = new(LocateRepositoryRoot);

	public static string Root => RootPath.Value;

	private static string LocateRepositoryRoot()
	{
		DirectoryInfo? directory = new(AppContext.BaseDirectory);
		while (directory is not null)
		{
			if (File.Exists(Path.Combine(directory.FullName, "linchpin.slnx")))
			{
				return directory.FullName;
			}

			directory = directory.Parent;
		}

		throw new InvalidOperationException("Unable to locate the Cornerstone repository root.");
	}
}