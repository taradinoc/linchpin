/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Linchpin.ToolTests;

[TestClass]
public sealed class LinchpinRuntimeTests
{
	public static IEnumerable<object[]> GetRuntimeCases() => RuntimeCaseCatalog.DynamicData();

	[TestMethod]
	[TestCategory("Linchpin")]
	public void CornerstoneConformanceMatchesExpectedScreenAndInstructionCount()
	{
		RuntimeCaseDefinition testCase = RuntimeCaseCatalog.AllCases.Single(static testCase => testCase.Name == "cornerstone-conformance");
		ToolRunOutcome outcome = ToolHarness.RunLinchpin(testCase);
		Assert.AreEqual(0, outcome.Process.ExitCode, ToolHarness.DescribeProcessFailure("Linchpin", outcome.Process));
		Assert.AreEqual(testCase.ExpectedInstructionCount, outcome.Report.ExecutedInstructionCount, ToolHarness.DescribeProcessFailure("Linchpin", outcome.Process));
		ToolHarness.AssertEquivalentScreenText(testCase.ExpectedScreenText, outcome.Report.ScreenText, ToolHarness.DescribeProcessFailure("Linchpin", outcome.Process));
	}

	[DataTestMethod]
	[DynamicData(nameof(GetRuntimeCases), DynamicDataSourceType.Method)]
	[TestCategory("Linchpin")]
	public void SharedRuntimeCaseMatchesExpectedScreenAndInstructionCount(RuntimeCaseDefinition testCase)
	{
		ToolRunOutcome outcome = ToolHarness.RunLinchpin(testCase);
		Assert.AreEqual(0, outcome.Process.ExitCode, ToolHarness.DescribeProcessFailure("Linchpin", outcome.Process));
		Assert.AreEqual(testCase.ExpectedInstructionCount, outcome.Report.ExecutedInstructionCount, ToolHarness.DescribeProcessFailure("Linchpin", outcome.Process));
		ToolHarness.AssertEquivalentScreenText(testCase.ExpectedScreenText, outcome.Report.ScreenText, ToolHarness.DescribeProcessFailure("Linchpin", outcome.Process));
	}
}