/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Linchpin.TestCaseTool;

internal static class RepositoryLayout
{
    private static readonly Lazy<string> RootPath = new(LocateRepositoryRoot);

    public static string Root => RootPath.Value;

    public static string CornerstoneDirectoryPath => Path.Combine(Root, "Cornerstone");

    public static string ShippedMmePath => Path.Combine(CornerstoneDirectoryPath, "CORNER.MME");

    public static string ShippedObjPath => Path.Combine(CornerstoneDirectoryPath, "CORNER.OBJ");

    public static string ShippedDataDirectoryPath => CornerstoneDirectoryPath;

    private static string LocateRepositoryRoot()
    {
        DirectoryInfo? directory = new(AppContext.BaseDirectory);
        while (directory is not null)
        {
            if (File.Exists(Path.Combine(directory.FullName, "cornerstone.sln")))
            {
                return directory.FullName;
            }

            directory = directory.Parent;
        }

        throw new ToolException("Unable to locate the Cornerstone repository root.");
    }
}