/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Linchpin.ToolTests;

internal sealed record RuntimeCaseOutcomes(
	ProcessResult? AssemblyResult,
	ToolRunOutcome? LinchpinOutcome,
	ToolRunOutcome? LinchpinStOutcome);

internal static class RuntimeCaseResults
{
	private static IReadOnlyDictionary<string, RuntimeCaseOutcomes>? _outcomes;

	/// <summary>
	/// Returns the precomputed outcomes for the named test case.
	/// Must only be called after <see cref="Initialize"/> has completed.
	/// </summary>
	public static RuntimeCaseOutcomes Get(string caseName)
	{
		if (_outcomes is null)
		{
			throw new InvalidOperationException("RuntimeCaseResults has not been initialized. Ensure AssemblyInitialize runs before tests.");
		}

		if (!_outcomes.TryGetValue(caseName, out RuntimeCaseOutcomes? outcomes))
		{
			throw new KeyNotFoundException($"No precomputed outcomes for runtime case '{caseName}'.");
		}

		return outcomes;
	}

	/// <summary>
	/// Assembles all Chisel-source cases, then runs every case under Linchpin and (if available)
	/// LinchpinST once each, caching the outcomes for later assertion by individual test methods.
	/// </summary>
	/// <param name="artifactsDirectory">Writable directory for assembled MME/OBJ artifacts.</param>
	public static void Initialize(string artifactsDirectory)
	{
		bool linchpinStAvailable = ToolHarness.IsLinchpinStAvailable;
		Dictionary<string, RuntimeCaseOutcomes> outcomes = new(StringComparer.Ordinal);

		foreach (RuntimeCaseDefinition tc in RuntimeCaseCatalog.AllCases)
		{
			ProcessResult? assemblyResult = null;
			RuntimeCaseDefinition runnableCase = tc;

			if (tc.ChiselSourcePath is not null)
			{
				string mmePath = Path.Combine(artifactsDirectory, $"{tc.Name}.mme");
				string objPath = Path.Combine(artifactsDirectory, $"{tc.Name}.obj");
				assemblyResult = ToolHarness.RunChiselAssemble(tc.ChiselSourcePath, mmePath, objPath);
				if (assemblyResult.ExitCode == 0)
				{
					runnableCase = tc with { ChiselSourcePath = null, MmePath = mmePath, ObjPath = objPath };
				}
			}

			bool canRun = assemblyResult is null || assemblyResult.ExitCode == 0;
			ToolRunOutcome? linchpinOutcome = null;
			ToolRunOutcome? linchpinStOutcome = null;

			if (canRun)
			{
				linchpinOutcome = ToolHarness.RunLinchpin(runnableCase);
				if (linchpinStAvailable)
				{
					linchpinStOutcome = ToolHarness.RunLinchpinSt(runnableCase);
				}
			}

			outcomes[tc.Name] = new RuntimeCaseOutcomes(assemblyResult, linchpinOutcome, linchpinStOutcome);
		}

		_outcomes = outcomes;
	}
}
