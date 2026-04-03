/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Linchpin.ToolTests;

[TestClass]
public sealed class RuntimeWorkspaceTests
{
	[TestMethod]
	[TestCategory("ToolHarness")]
	public void ExplicitDataDirectoryDoesNotMergeCompanionSampleDirectory()
	{
		string rootPath = Path.Combine(Path.GetTempPath(), $"cornerstone-runtimeworkspace-test-{Guid.NewGuid():N}");
		Directory.CreateDirectory(rootPath);

		try
		{
			string explicitDataDirectory = Path.Combine(rootPath, "DB", "FUNK");
			Directory.CreateDirectory(explicitDataDirectory);
			File.WriteAllText(Path.Combine(explicitDataDirectory, "MATH.DBF"), "from-explicit-data-dir");

			string imageDirectory = Path.Combine(rootPath, "Cornerstone");
			Directory.CreateDirectory(imageDirectory);
			string mmePath = Path.Combine(imageDirectory, "CORNER.MME");
			File.WriteAllText(mmePath, string.Empty);

			string companionSampleDirectory = Path.Combine(rootPath, "Sample");
			Directory.CreateDirectory(companionSampleDirectory);
			File.WriteAllText(Path.Combine(companionSampleDirectory, "MATH.DBF"), "from-companion-sample");
			File.WriteAllText(Path.Combine(companionSampleDirectory, "CUSTOMER.DBF"), "sample-only");

			using RuntimeWorkspace workspace = RuntimeWorkspace.Create("explicit-data-dir", explicitDataDirectory, mmePath);
			Assert.IsNotNull(workspace.SampleDirectoryPath);

			string copiedMathPath = Path.Combine(workspace.SampleDirectoryPath!, "MATH.DBF");
			string leakedSampleOnlyPath = Path.Combine(workspace.SampleDirectoryPath!, "CUSTOMER.DBF");

			Assert.AreEqual("from-explicit-data-dir", File.ReadAllText(copiedMathPath));
			Assert.IsFalse(File.Exists(leakedSampleOnlyPath), "Companion Sample files should not be copied when an explicit data directory is provided.");
		}
		finally
		{
			if (Directory.Exists(rootPath))
			{
				Directory.Delete(rootPath, recursive: true);
			}
		}
	}
}