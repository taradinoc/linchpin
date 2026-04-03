/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Linchpin;

/// <summary>
/// Holds the entire mutable state of a running Cornerstone VM: evaluation stack, call stack,
/// RAM, globals, file I/O channels, and display host. This partial class is split across
/// several files grouped by subsystem (Core, Tracing, MemoryAggregates, FileIO, ExtractDisplay).
/// </summary>
internal sealed partial class VmRuntimeState
{
}
