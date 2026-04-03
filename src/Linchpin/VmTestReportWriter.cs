using System.Text.Json;

namespace Linchpin;

/// <summary>
/// Writes a VM execution result to a JSON test report file.
/// </summary>
internal static class VmTestReportWriter
{
	private sealed record ReportModel(
		int HaltCode,
		int ExecutedInstructionCount,
		string ScreenText,
		bool StoppedByInstructionLimit,
		string StopReason,
		string? StopDetail);

	private static readonly JsonSerializerOptions Options = new()
	{
		PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
		WriteIndented = true,
	};

	/// <summary>
	/// Serializes the execution result to a JSON file at the specified path.
	/// </summary>
	/// <param name="path">The output file path.</param>
	/// <param name="result">The VM execution result to write.</param>
	public static void Write(string path, VmExecutionResult result)
	{
		string fullPath = Path.GetFullPath(path);
		string? directory = Path.GetDirectoryName(fullPath);
		if (!string.IsNullOrWhiteSpace(directory))
		{
			Directory.CreateDirectory(directory);
		}

		ReportModel report = new(
			result.HaltCode,
			result.ExecutedInstructionCount,
			NormalizeScreenText(result.ScreenText),
			result.StoppedByInstructionLimit,
			result.StopReason.ToString(),
			result.StopDetail);
		File.WriteAllText(fullPath, JsonSerializer.Serialize(report, Options));
	}

	private static string NormalizeScreenText(string screenText) =>
		string.IsNullOrEmpty(screenText)
			? string.Empty
			: screenText.Replace("\r\n", "\n", StringComparison.Ordinal);
}