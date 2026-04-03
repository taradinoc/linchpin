/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Linchpin.ToolTests;

[TestClass]
public sealed class LinchpinStRuntimeTests
{
	public static IEnumerable<object[]> GetRuntimeCases() => RuntimeCaseCatalog.DynamicData();

	[DataTestMethod]
	[DynamicData(nameof(GetRuntimeCases), DynamicDataSourceType.Method)]
	[TestCategory("LinchpinST")]
	[TestCategory("WSL")]
	public void SharedRuntimeCaseMatchesExpectedScreenAndInstructionCount(RuntimeCaseDefinition testCase)
	{
		ToolHarness.RequireLinchpinSt();
		ToolRunOutcome outcome = ToolHarness.RunLinchpinSt(testCase);
		Assert.AreEqual(0, outcome.Process.ExitCode, ToolHarness.DescribeProcessFailure("LinchpinST", outcome.Process));
		Assert.AreEqual(testCase.ExpectedInstructionCount, outcome.Report.ExecutedInstructionCount, ToolHarness.DescribeProcessFailure("LinchpinST", outcome.Process));
		ToolHarness.AssertEquivalentScreenText(testCase.ExpectedScreenText, outcome.Report.ScreenText, ToolHarness.DescribeProcessFailure("LinchpinST", outcome.Process));
	}

	[DataTestMethod]
	[DynamicData(nameof(GetRuntimeCases), DynamicDataSourceType.Method)]
	[TestCategory("LinchpinST")]
	[TestCategory("WSL")]
	public void SharedRuntimeCaseMatchesLinchpinParity(RuntimeCaseDefinition testCase)
	{
		ToolHarness.RequireLinchpinSt();
		ToolRunOutcome linchpin = ToolHarness.RunLinchpin(testCase);
		ToolRunOutcome linchpinSt = ToolHarness.RunLinchpinSt(testCase);
		Assert.AreEqual(0, linchpin.Process.ExitCode, ToolHarness.DescribeProcessFailure("Linchpin", linchpin.Process));
		Assert.AreEqual(0, linchpinSt.Process.ExitCode, ToolHarness.DescribeProcessFailure("LinchpinST", linchpinSt.Process));
		Assert.AreEqual(linchpin.Report.ExecutedInstructionCount, linchpinSt.Report.ExecutedInstructionCount, ToolHarness.DescribeProcessFailure("LinchpinST", linchpinSt.Process));
		ToolHarness.AssertEquivalentScreenText(linchpin.Report.ScreenText, linchpinSt.Report.ScreenText, ToolHarness.DescribeProcessFailure("LinchpinST", linchpinSt.Process));
	}
}