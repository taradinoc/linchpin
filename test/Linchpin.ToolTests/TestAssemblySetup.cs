/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Linchpin.ToolTests;

[TestClass]
public sealed class TestAssemblySetup
{
	private static string? _artifactsDirectory;

	[AssemblyInitialize]
	public static void Initialize(TestContext _)
	{
		_artifactsDirectory = Path.Combine(Path.GetTempPath(), $"linchpin-tests-{Guid.NewGuid():N}");
		Directory.CreateDirectory(_artifactsDirectory);
		RuntimeCaseResults.Initialize(_artifactsDirectory);
	}

	[AssemblyCleanup]
	public static void Cleanup()
	{
		if (_artifactsDirectory is not null && Directory.Exists(_artifactsDirectory))
		{
			Directory.Delete(_artifactsDirectory, recursive: true);
		}
	}
}
