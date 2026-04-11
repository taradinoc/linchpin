/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Linchpin.ToolTests;

[TestClass]
public sealed class LinchpinRuntimeTests
{
	public static IEnumerable<object[]> GetRuntimeCases() => RuntimeCaseCatalog.DynamicData();

	[DataTestMethod]
	[DynamicData(nameof(GetRuntimeCases), DynamicDataSourceType.Method)]
	[TestCategory("Linchpin")]
	public void SharedRuntimeCaseMatchesExpectedScreenAndInstructionCount(RuntimeCaseDefinition testCase)
	{
		RuntimeCaseOutcomes outcomes = RuntimeCaseResults.Get(testCase.Name);
		if (outcomes.LinchpinOutcome is null)
		{
			Assert.Fail($"Linchpin did not run for case '{testCase.Name}' (assembly may have failed).");
			return;
		}

		ToolRunOutcome outcome = outcomes.LinchpinOutcome;
		Assert.AreEqual(0, outcome.Process.ExitCode, ToolHarness.DescribeProcessFailure("Linchpin", outcome.Process));
		Assert.AreEqual(testCase.ExpectedInstructionCount, outcome.Report.ExecutedInstructionCount, ToolHarness.DescribeProcessFailure("Linchpin", outcome.Process));
		ToolHarness.AssertEquivalentScreenText(testCase.ExpectedScreenText, outcome.Report.ScreenText, ToolHarness.DescribeProcessFailure("Linchpin", outcome.Process));
	}
}