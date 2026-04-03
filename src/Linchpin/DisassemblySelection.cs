/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Linchpin;

/// <summary>
/// Filters disassembly output to a specific module and/or procedure.
/// When both values are <c>null</c>, the entire image is included.
/// </summary>
internal sealed record DisassemblySelection(int? ModuleId, int? ProcedureIndex)
{
	public bool IncludesModule(ModuleImage module) => !ModuleId.HasValue || module.ModuleId == ModuleId.Value;

	public bool IncludesProcedure(ProcedureDisassembly procedure) =>
		(!ModuleId.HasValue || procedure.Key.ModuleId == ModuleId.Value)
		&& (!ProcedureIndex.HasValue || procedure.Procedure.ExportedProcedureIndex == ProcedureIndex.Value);

	public string Describe() => ProcedureIndex.HasValue
		? $"module {ModuleId}, procedure {ProcedureIndex}"
		: ModuleId.HasValue
			? $"module {ModuleId}"
			: "whole image";
}