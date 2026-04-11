/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Linchpin.ToolTests;

[TestClass]
public sealed class ChiselIntegrationTests
{
	public static IEnumerable<object[]> GetChiselCases() => RuntimeCaseCatalog.AllCases.Where(testCase => testCase.ChiselSourcePath is not null).Select(testCase => new object[] { testCase });

	[TestMethod]
	[TestCategory("Chisel")]
	public void AssembleReportsStableFailureForMissingEndproc()
	{
		using RuntimeWorkspace workspace = RuntimeWorkspace.Create("chisel-missing-endproc", null);
		string mmePath = Path.Combine(workspace.RootPath, "missing-endproc.mme");
		string objPath = Path.Combine(workspace.RootPath, "missing-endproc.obj");
		string sourcePath = Path.Combine(RepositoryLayout.Root, "test", "Linchpin.ToolTests", "TestData", "chisel", "missing-endproc.cas");
		ProcessResult process = ToolHarness.RunChiselAssemble(sourcePath, mmePath, objPath);
		Assert.AreEqual(1, process.ExitCode, ToolHarness.DescribeProcessFailure("Chisel", process));
		StringAssert.Contains(process.StandardError, "missing .endproc");
	}

	[TestMethod]
	[TestCategory("Chisel")]
	public void AssembleTreatsJUMPLEAsBinaryCompare()
	{
		using RuntimeWorkspace workspace = RuntimeWorkspace.Create("chisel-jumple-binary", null);
		string mmePath = Path.Combine(workspace.RootPath, "jumple-binary.mme");
		string objPath = Path.Combine(workspace.RootPath, "jumple-binary.obj");
		string sourcePath = Path.Combine(RepositoryLayout.Root, "test", "Linchpin.ToolTests", "TestData", "chisel", "jumple-binary.cas");

		ProcessResult assemble = ToolHarness.RunChiselAssemble(sourcePath, mmePath, objPath);
		Assert.AreEqual(0, assemble.ExitCode, ToolHarness.DescribeProcessFailure("Chisel", assemble));

		RuntimeCaseDefinition testCase = new(
			"chisel-jumple-binary",
			mmePath,
			objPath,
			null,
			null,
			string.Empty,
			0,
			32,
			string.Empty);

		ToolRunOutcome run = ToolHarness.RunLinchpin(testCase, workspace, mmePath, objPath);
		Assert.AreEqual(0, run.Process.ExitCode, ToolHarness.DescribeProcessFailure("Linchpin", run.Process));
		Assert.AreEqual(0, run.Report.HaltCode, ToolHarness.DescribeProcessFailure("Linchpin", run.Process));
	}

	[DataTestMethod]
	[DynamicData(nameof(GetChiselCases), DynamicDataSourceType.Method)]
	[TestCategory("Chisel")]
	public void AssembleAndRunMatchesSharedRuntimeExpectation(RuntimeCaseDefinition testCase)
	{
		Assert.IsNotNull(testCase.ChiselSourcePath, "Chisel test cases must define a source path.");
		RuntimeCaseOutcomes outcomes = RuntimeCaseResults.Get(testCase.Name);
		Assert.IsNotNull(outcomes.AssemblyResult, "Expected an assembly result for a Chisel test case.");
		Assert.AreEqual(0, outcomes.AssemblyResult.ExitCode, ToolHarness.DescribeProcessFailure("Chisel", outcomes.AssemblyResult));
		if (outcomes.LinchpinOutcome is null)
		{
			Assert.Fail($"Linchpin did not run for case '{testCase.Name}' (assembly may have failed).");
			return;
		}

		ToolRunOutcome run = outcomes.LinchpinOutcome;
		Assert.AreEqual(0, run.Process.ExitCode, ToolHarness.DescribeProcessFailure("Linchpin", run.Process));
		Assert.AreEqual(testCase.ExpectedInstructionCount, run.Report.ExecutedInstructionCount, ToolHarness.DescribeProcessFailure("Linchpin", run.Process));
		ToolHarness.AssertEquivalentScreenText(testCase.ExpectedScreenText, run.Report.ScreenText, ToolHarness.DescribeProcessFailure("Linchpin", run.Process));
	}
}