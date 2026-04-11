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
		RuntimeCaseOutcomes outcomes = RuntimeCaseResults.Get(testCase.Name);
		if (outcomes.LinchpinStOutcome is null)
		{
			ToolHarness.RequireLinchpinSt();
			Assert.Fail($"LinchpinST did not run for case '{testCase.Name}' (assembly may have failed).");
			return;
		}

		ToolRunOutcome outcome = outcomes.LinchpinStOutcome;
		Assert.AreEqual(0, outcome.Process.ExitCode, ToolHarness.DescribeProcessFailure("LinchpinST", outcome.Process));
		Assert.AreEqual(testCase.ExpectedInstructionCount, outcome.Report.ExecutedInstructionCount, ToolHarness.DescribeProcessFailure("LinchpinST", outcome.Process));
		ToolHarness.AssertEquivalentScreenText(testCase.ExpectedScreenText, outcome.Report.ScreenText, ToolHarness.DescribeProcessFailure("LinchpinST", outcome.Process));
	}
}