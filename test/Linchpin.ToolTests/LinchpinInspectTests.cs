/* Copyright 2026 Tara McGrew. See LICENSE for details. */

using System.Text.RegularExpressions;
using System.Text.Json;
using Microsoft.VisualStudio.TestTools.UnitTesting;

namespace Linchpin.ToolTests;

[TestClass]
public sealed partial class LinchpinInspectTests
{
	[TestMethod]
	[TestCategory("Linchpin")]
	public void InspectShippedEntryPreviewStartsAtCodeOffset()
	{
		ProcessResult process = ToolHarness.RunLinchpinInspectShipped();
		Assert.AreEqual(0, process.ExitCode, ToolHarness.DescribeProcessFailure("Linchpin inspect", process));

		Match match = EntryPreviewRegex().Match(process.StandardOutput);
		Assert.IsTrue(match.Success, $"Unable to find entry preview block in inspect output.{Environment.NewLine}{process.StandardOutput}");

		int startOffset = Convert.ToInt32(match.Groups["start"].Value, 16);
		int codeOffset = Convert.ToInt32(match.Groups["code"].Value, 16);
		int firstInstructionOffset = Convert.ToInt32(match.Groups["first"].Value, 16);

		Assert.AreNotEqual(startOffset, codeOffset, "The shipped entry procedure should have a non-empty header for this regression test.");
		Assert.AreEqual(codeOffset, firstInstructionOffset, "Inspect preview should begin decoding at the procedure code offset, not the header start.");
		Assert.AreNotEqual(startOffset, firstInstructionOffset, "Inspect preview must not decode the procedure header as an opcode.");
	}

	[GeneratedRegex(@"Entry procedure preview: module \d+, proc \d+, start 0x(?<start>[0-9A-F]+), code 0x(?<code>[0-9A-F]+)\r?\n\s+0x(?<first>[0-9A-F]+):", RegexOptions.CultureInvariant)]
	private static partial Regex EntryPreviewRegex();

	[TestMethod]
	[TestCategory("Linchpin")]
	public void DisassembleShippedKeepsAdjacentPrivateProcedureHeadersOutOfPreviousProcedure()
	{
		string jsonPath = Path.Combine(Path.GetTempPath(), $"linchpin-disassemble-{Guid.NewGuid():N}.json");
		try
		{
			ProcessResult process = ToolHarness.RunLinchpinDisassembleShipped(2, jsonPath);
			Assert.AreEqual(0, process.ExitCode, ToolHarness.DescribeProcessFailure("Linchpin disassemble", process));

			using JsonDocument document = JsonDocument.Parse(File.ReadAllText(jsonPath));
			JsonElement procedures = document.RootElement
				.GetProperty("modules")[0]
				.GetProperty("procedures");

			JsonElement proc089 = FindProcedure(procedures, "Proc089");
			JsonElement procPriv3B64 = FindProcedure(procedures, "ProcPriv_3B64");
			JsonElement procPriv3BCE = FindProcedure(procedures, "ProcPriv_3BCE");

			Assert.AreEqual(0x3B64, proc089.GetProperty("upperBound").GetInt32(), "Proc089 should stop at the next private procedure start.");
			Assert.AreEqual(0x3B64, procPriv3B64.GetProperty("startOffset").GetInt32());
			Assert.AreEqual(0x3B65, procPriv3B64.GetProperty("codeOffset").GetInt32());
			Assert.AreEqual(0x3BCE, procPriv3B64.GetProperty("upperBound").GetInt32(), "ProcPriv_3B64 should stop at the following private procedure start.");
			Assert.AreEqual(0x3BCE, procPriv3BCE.GetProperty("startOffset").GetInt32());
			Assert.AreEqual(0x3BCF, procPriv3BCE.GetProperty("codeOffset").GetInt32());

			int firstInstructionOffset = procPriv3B64.GetProperty("instructions")[0].GetProperty("offset").GetInt32();
			Assert.AreEqual(0x3B65, firstInstructionOffset, "Private procedure disassembly must start at codeOffset, not the header byte.");
		}
		finally
		{
			if (File.Exists(jsonPath))
			{
				File.Delete(jsonPath);
			}
		}
	}

	[TestMethod]
	[TestCategory("Linchpin")]
	public void DisassembleShippedDiscoversPushConstantThenCallPrivateProcedure()
	{
		string jsonPath = Path.Combine(Path.GetTempPath(), $"linchpin-disassemble-{Guid.NewGuid():N}.json");
		try
		{
			ProcessResult process = ToolHarness.RunLinchpinDisassembleShipped(2, jsonPath);
			Assert.AreEqual(0, process.ExitCode, ToolHarness.DescribeProcessFailure("Linchpin disassemble", process));

			using JsonDocument document = JsonDocument.Parse(File.ReadAllText(jsonPath));
			JsonElement procedures = document.RootElement
				.GetProperty("modules")[0]
				.GetProperty("procedures");

			JsonElement procPriv08DF = FindProcedure(procedures, "ProcPriv_08DF");

			Assert.AreEqual(0x08DF, procPriv08DF.GetProperty("startOffset").GetInt32());
			Assert.AreEqual(0x08E0, procPriv08DF.GetProperty("codeOffset").GetInt32());

			int firstInstructionOffset = procPriv08DF.GetProperty("instructions")[0].GetProperty("offset").GetInt32();
			Assert.AreEqual(0x08E0, firstInstructionOffset, "Recovered private procedure must still decode from its code offset, not its header start.");

			bool hasDirectIncomingCall = procPriv08DF.GetProperty("incomingCalls")
				.EnumerateArray()
				.Any(call => call.GetProperty("mnemonic").GetString() == "CALL"
					&& call.GetProperty("isDirect").GetBoolean());
			Assert.IsTrue(hasDirectIncomingCall, "The PUSH-constant-then-CALL sequence should be recognized as a direct near call to ProcPriv_08DF.");
		}
		finally
		{
			if (File.Exists(jsonPath))
			{
				File.Delete(jsonPath);
			}
		}
	}

	private static JsonElement FindProcedure(JsonElement procedures, string name)
	{
		foreach (JsonElement procedure in procedures.EnumerateArray())
		{
			if (procedure.GetProperty("name").GetString() == name)
			{
				return procedure;
			}
		}

		Assert.Fail($"Procedure '{name}' was not present in Linchpin disassembly output.");
		return default;
	}
}