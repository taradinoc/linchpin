/* Copyright 2026 Tara McGrew. See LICENSE for details. */

namespace Linchpin;

/// <summary>
/// Emulates an 80×25 text-mode screen for the Cornerstone VM. In transcript mode, the screen
/// buffer is maintained silently; in live console mode, changes are painted to the real console.
/// Also handles keyboard input mapping from .NET console keys to Cornerstone's DOS-style scan codes.
/// </summary>
internal sealed class VirtualScreenHost : IDisposable
{
	private const ushort FalseSentinel = 0x8001;
	private const int Width = 80;
	private const int Height = 25;
	private const byte MonoNormalBaseAttribute = 0x07;
	private const byte ColorNormalBaseAttribute = 0x17;
	private const byte DefaultReverseBaseAttribute = 0x70;
	private const byte BrightMaskAttribute = 0x08;
	private const byte BlinkMaskAttribute = 0x80;
	private static readonly IReadOnlyDictionary<ConsoleKey, ushort> ExtendedConsoleKeyMap = new Dictionary<ConsoleKey, ushort>
	{
		[ConsoleKey.UpArrow] = 0x48,
		[ConsoleKey.DownArrow] = 0x50,
		[ConsoleKey.LeftArrow] = 0x4B,
		[ConsoleKey.RightArrow] = 0x4D,
		[ConsoleKey.Insert] = 0x52,
		[ConsoleKey.Delete] = 0x53,
		[ConsoleKey.Home] = 0x47,
		[ConsoleKey.End] = 0x4F,
		[ConsoleKey.PageUp] = 0x49,
		[ConsoleKey.PageDown] = 0x51,
		[ConsoleKey.F1] = 0x3B,
		[ConsoleKey.F2] = 0x3C,
		[ConsoleKey.F3] = 0x3D,
		[ConsoleKey.F4] = 0x3E,
		[ConsoleKey.F5] = 0x3F,
		[ConsoleKey.F6] = 0x40,
		[ConsoleKey.F7] = 0x41,
		[ConsoleKey.F8] = 0x42,
		[ConsoleKey.F9] = 0x43,
		[ConsoleKey.F10] = 0x44,
		[ConsoleKey.F11] = 0x85,
		[ConsoleKey.F12] = 0x86,
	};
	private static readonly IReadOnlyDictionary<ConsoleKey, ushort> ControlExtendedConsoleKeyMap = new Dictionary<ConsoleKey, ushort>
	{
		[ConsoleKey.F2] = 0x5F,
		[ConsoleKey.Insert] = 0x92,
		[ConsoleKey.Delete] = 0x93,
		[ConsoleKey.Home] = 0x77,
		[ConsoleKey.End] = 0x75,
		[ConsoleKey.PageUp] = 0x84,
		[ConsoleKey.PageDown] = 0x76,
		[ConsoleKey.UpArrow] = 0x8D,
		[ConsoleKey.DownArrow] = 0x91,
		[ConsoleKey.LeftArrow] = 0x73,
		[ConsoleKey.RightArrow] = 0x74,
	};
	private readonly ScreenCell[,] cells = new ScreenCell[Height, Width];
	private readonly Queue<ushort> inputQueue;
	private readonly ConsoleColor originalForegroundColor;
	private readonly ConsoleColor originalBackgroundColor;
	private bool liveConsoleEnabled;
	private bool disposed;
	private bool originalCursorVisible;
	private bool inputCursorVisible;
	private int currentRow;
	private int currentColumn;
	private int visibleCursorRow;
	private int visibleCursorColumn;
	private byte currentDisplayAttribute = MonoNormalBaseAttribute;
	private byte normalBaseAttribute = MonoNormalBaseAttribute;
	private ushort lastStyleBits;
	private ushort rowSlot;
	private ushort columnSlot;
	private ushort scrollRowTop;
	private ushort scrollRowBottom = Height - 1;
	private bool renderCacheDirty = true;
	private string cachedRender = string.Empty;
	private int consoleWidth;
	private int consoleHeight;
	private int lastConsoleCursorRow = -1;
	private int lastConsoleCursorColumn = -1;
	private byte? lastConsoleAttribute;
	private bool? lastCursorVisible;

	private readonly record struct ScreenCell(char Character, byte Attribute);


	public VirtualScreenHost(string inputText, VmRunDisplayMode displayMode)
	{
		liveConsoleEnabled = displayMode == VmRunDisplayMode.LiveConsole;
		inputQueue = new Queue<ushort>(inputText.Select(character => (ushort)character));
		originalForegroundColor = TryGetConsoleForegroundColor();
		originalBackgroundColor = TryGetConsoleBackgroundColor();
		ClearAll(repaintConsole: false);
		InitializeLiveConsole();
	}

	/// <summary>
	/// Intercepts writes to display-related module global slots, updating cursor position,
	/// scroll region, display style, and color mode accordingly.
	/// </summary>
	/// <param name="index">The module global slot index.</param>
	/// <param name="value">The value being written.</param>
	public void NotifyModuleGlobalWrite(int index, ushort value)
	{
		switch (index)
		{
			case 0xC4:
				scrollRowBottom = value;
				break;
			case 0xC5:
				scrollRowTop = value;
				break;
			case 0xCA:
				rowSlot = value;
				break;
			case 0xC9:
				columnSlot = value;
				CommitCursor();
				break;
			case 0xD5:
				lastStyleBits = value;
				currentDisplayAttribute = ComputeDisplayAttribute(value);
				break;
			case 0xDB:
				normalBaseAttribute = (value & 1) != 0 ? ColorNormalBaseAttribute : MonoNormalBaseAttribute;
				currentDisplayAttribute = ComputeDisplayAttribute(lastStyleBits);
				break;
		}
	}

	/// <summary>
	/// Executes a DISP sub-operation: cursor movement (up/down/left/right), erase to end of line,
	/// or clear from cursor to bottom.
	/// </summary>
	public void ApplyDisplaySuboperation(int suboperation)
	{
		switch (suboperation)
		{
			case 0x00:
				MoveCursor(0, 1);
				break;
			case 0x01:
				MoveCursor(0, -1);
				break;
			case 0x02:
				MoveCursor(1, 0);
				break;
			case 0x03:
				MoveCursor(-1, 0);
				break;
			case 0x04:
				EraseToEndOfLine();
				break;
			case 0x05:
				ClearFromCursorToBottom();
				break;
			default:
				throw new LinchpinException($"DISP 0x{suboperation:X2} is not implemented in the current host subset.");
		}
	}

	/// <summary>
	/// Executes an XDISP extended display sub-operation: scroll region up/down, draw horizontal line.
	/// </summary>
	public void ApplyExtendedDisplaySuboperation(int suboperation, ushort argument)
	{
		int lineCount = argument;
		switch (suboperation)
		{
			case 0x00:
                ScrollRegion(currentRow, ClampRow(scrollRowBottom), lineCount, down: false);
				break;
			case 0x01:
                ScrollRegion(currentRow, ClampRow(scrollRowBottom), lineCount, down: true);
				break;
			case 0x02:
			case 0x03:
				break;
			case 0x04:
                ScrollRegion(ClampRow(scrollRowTop), ClampRow(scrollRowBottom), lineCount, down: false);
				break;
			case 0x05:
                ScrollRegion(ClampRow(scrollRowTop), ClampRow(scrollRowBottom), lineCount, down: true);
				break;
			case 0x06:
				DrawHorizontalLine(argument);
				break;
			default:
				throw new LinchpinException($"XDISP 0x{suboperation:X2} is not implemented in the current host subset.");
		}
	}

	/// <summary>
	/// Prints a range of bytes from a VM aggregate vector as ASCII characters to the screen.
	/// </summary>
	public void PrintVector(VmRuntimeState state, ushort handle, ushort length, ushort startOffset)
	{
		foreach (byte value in state.ReadAggregatePayloadBytes(handle, length, startOffset))
		{
			char printable = value >= 0x20 && value < 0x7F
				? (char)value
				: ' ';
			PrintCharacter(printable);
		}
	}

	/// <summary>
	/// Writes a single character to the screen at the current cursor position and advances the cursor.
	/// </summary>
	public void PrintCharacter(char character)
	{
		if (currentRow < 0 || currentRow >= Height)
		{
			return;
		}

		MarkRenderDirty();
		if (currentColumn < 0)
		{
			currentColumn = 0;
		}

		if (currentColumn >= Width)
		{
			currentColumn = Width - 1;
		}

		int row = currentRow;
		int column = currentColumn;
		cells[currentRow, currentColumn] = new ScreenCell(character, currentDisplayAttribute);
		WriteConsoleCell(row, column, cells[row, column]);
		if (currentColumn < Width - 1)
		{
			currentColumn++;
			columnSlot = (ushort)currentColumn;
		}
	}

	public void SetCursor(int row, int column)
	{
		currentRow = Math.Clamp(row, 0, Height - 1);
		currentColumn = Math.Clamp(column, 0, Width - 1);
		rowSlot = (ushort)currentRow;
		columnSlot = (ushort)currentColumn;
		if (inputCursorVisible)
		{
			SyncLiveConsoleCursor();
		}
	}

	/// <summary>
	/// Returns the next key from the input queue, reading interactively from the console if necessary.
	/// Returns <c>FalseSentinel</c> if no key is available.
	/// </summary>
	public ushort PollKey()
	{
		UpdateVisibleCursorFromCurrent();

		if (inputQueue.Count > 0)
		{
			return inputQueue.Dequeue();
		}

		if (!TryReadInteractiveKey())
		{
			if (liveConsoleEnabled)
			{
				Thread.Sleep(1);
			}

			return FalseSentinel;
		}

		return inputQueue.Count == 0 ? FalseSentinel : inputQueue.Dequeue();
	}

	/// <summary>
	/// Returns the current state of NumLock and CapsLock as a bitmask (Windows only).
	/// </summary>
	public ushort GetLockKeyState()
	{
		ushort state = 0;
		if (!liveConsoleEnabled || !OperatingSystem.IsWindows())
		{
			return state;
		}

		try
		{
			if (Console.NumberLock)
			{
				state |= 0x0001;
			}

			if (Console.CapsLock)
			{
				state |= 0x0002;
			}
		}
		catch (InvalidOperationException)
		{
		}
		catch (PlatformNotSupportedException)
		{
		}

		return state;
	}

	public (ushort Month, ushort Day, ushort Year) GetCurrentDate()
	{
		DateTime now = DateTime.Now;
		return ((ushort)now.Month, (ushort)now.Day, (ushort)now.Year);
	}

	public (ushort Hour, ushort Minute, ushort Second) GetCurrentTime()
	{
		DateTime now = DateTime.Now;
		return ((ushort)now.Hour, (ushort)now.Minute, (ushort)now.Second);
	}

	/// <summary>
	/// Returns the screen contents as a multi-line string with trailing whitespace trimmed.
	/// Uses a cached result that is invalidated whenever the screen buffer changes.
	/// </summary>
	public string Render()
	{
		if (!renderCacheDirty)
		{
			return cachedRender;
		}

		List<string> lines = new();
		for (int row = 0; row < Height; row++)
		{
			char[] line = new char[Width];
			for (int column = 0; column < Width; column++)
			{
				line[column] = cells[row, column].Character;
			}

			lines.Add(new string(line).TrimEnd());
		}

		int lastNonEmpty = lines.FindLastIndex(line => line.Length > 0);
		cachedRender = lastNonEmpty < 0
			? string.Empty
			: string.Join(Environment.NewLine, lines.Take(lastNonEmpty + 1));
		renderCacheDirty = false;
		return cachedRender;
	}

	public bool ContainsRenderedText(string text)
	{
		return Render().Contains(text, StringComparison.Ordinal);
	}

	public void Dispose()
	{
		if (disposed)
		{
			return;
		}

		disposed = true;
		if (!liveConsoleEnabled)
		{
			return;
		}

		TrySetConsoleColors(originalForegroundColor, originalBackgroundColor);
		TryRefreshConsoleMetrics();
		TrySetConsoleCursorPosition(0, Math.Min(Height, consoleHeight - 1));
		TrySetCursorVisible(originalCursorVisible);
		Console.WriteLine();
	}

	private void ClearAll(bool repaintConsole = true)
	{
		MarkRenderDirty();
		for (int row = 0; row < Height; row++)
		{
			for (int column = 0; column < Width; column++)
			{
				cells[row, column] = new ScreenCell(' ', currentDisplayAttribute);
			}
		}

		if (repaintConsole)
		{
			RepaintRows(0, Height - 1);
		}
	}

	private void ClearFromCursorToBottom()
	{
		MarkRenderDirty();
		for (int row = currentRow; row < Height; row++)
		{
			int startColumn = row == currentRow ? currentColumn : 0;
			for (int column = startColumn; column < Width; column++)
			{
				cells[row, column] = new ScreenCell(' ', currentDisplayAttribute);
			}
		}

		RepaintRows(currentRow, Height - 1);
		SyncLiveConsoleCursor();
	}

	private void EraseToEndOfLine()
	{
		if (currentRow < 0 || currentRow >= Height)
		{
			return;
		}

		MarkRenderDirty();
		int startColumn = Math.Clamp(currentColumn, 0, Width - 1);
		for (int column = startColumn; column < Width; column++)
		{
			cells[currentRow, column] = new ScreenCell(' ', currentDisplayAttribute);
		}

		RepaintRows(currentRow, currentRow);
		SyncLiveConsoleCursor();
	}

	private void CommitCursor()
	{
		currentRow = Math.Clamp(rowSlot, (ushort)0, (ushort)(Height - 1));
		currentColumn = Math.Clamp(columnSlot, (ushort)0, (ushort)(Width - 1));
		SyncLiveConsoleCursor();
	}

	private static int ClampRow(ushort row)
	{
		return Math.Clamp(row, (ushort)0, (ushort)(Height - 1));
	}

	/// <summary>
	/// Scrolls a range of rows up or down, clearing the vacated lines.
	/// </summary>
	private void ScrollRegion(int topRow, int bottomRow, int lineCount, bool down)
	{
		MarkRenderDirty();
		if (topRow > bottomRow)
		{
			(topRow, bottomRow) = (bottomRow, topRow);
		}

		topRow = Math.Clamp(topRow, 0, Height - 1);
		bottomRow = Math.Clamp(bottomRow, 0, Height - 1);
		lineCount = Math.Clamp(lineCount, 0, bottomRow - topRow + 1);

		if (lineCount == 0)
		{
			ClearRegion(topRow, bottomRow);
			RepaintRows(topRow, bottomRow);
			SyncLiveConsoleCursor();
			return;
		}

		if (down)
		{
			for (int row = bottomRow; row >= topRow; row--)
			{
				int sourceRow = row - lineCount;
				if (sourceRow >= topRow)
				{
					CopyRow(sourceRow, row);
				}
				else
				{
					ClearRow(row);
				}
			}
		}
		else
		{
			for (int row = topRow; row <= bottomRow; row++)
			{
				int sourceRow = row + lineCount;
				if (sourceRow <= bottomRow)
				{
					CopyRow(sourceRow, row);
				}
				else
				{
					ClearRow(row);
				}
			}
		}

		int clearedTop = down ? topRow : Math.Max(topRow, bottomRow - lineCount + 1);
		int clearedBottom = down ? Math.Min(bottomRow, topRow + lineCount - 1) : bottomRow;

		if (TryScrollConsoleRegion(topRow, bottomRow, lineCount, down))
		{
			RepaintRows(clearedTop, clearedBottom);
			SyncLiveConsoleCursor();
			return;
		}

		RepaintRows(topRow, bottomRow);
		SyncLiveConsoleCursor();
	}

	private bool TryScrollConsoleRegion(int topRow, int bottomRow, int lineCount, bool down)
	{
		if (!liveConsoleEnabled || !OperatingSystem.IsWindows())
		{
			return false;
		}

		int consoleWidth = Math.Min(Width, GetConsoleWidth());
		int consoleHeight = GetConsoleHeight();
		if (consoleWidth <= 0 || consoleHeight <= 0)
		{
			return false;
		}

		int clampedTop = Math.Clamp(topRow, 0, Math.Min(Height - 1, consoleHeight - 1));
		int clampedBottom = Math.Clamp(bottomRow, 0, Math.Min(Height - 1, consoleHeight - 1));
		if (clampedTop > clampedBottom)
		{
			(clampedTop, clampedBottom) = (clampedBottom, clampedTop);
		}

		int rowCount = clampedBottom - clampedTop + 1;
		if (rowCount <= 0 || lineCount <= 0 || lineCount >= rowCount)
		{
			return false;
		}

		int sourceTop = down ? clampedTop : clampedTop + lineCount;
		int sourceHeight = rowCount - lineCount;
		int targetTop = down ? clampedTop + lineCount : clampedTop;

		if (sourceHeight <= 0 || targetTop < 0 || targetTop >= consoleHeight)
		{
			return false;
		}

		try
		{
			Console.MoveBufferArea(0, sourceTop, consoleWidth, sourceHeight, 0, targetTop);
			return true;
		}
		catch (PlatformNotSupportedException)
		{
			return false;
		}
		catch (ArgumentOutOfRangeException)
		{
			return false;
		}
		catch (ArgumentException)
		{
			return false;
		}
		catch (IOException)
		{
			return false;
		}
	}

	private void DrawHorizontalLine(int width)
	{
		MarkRenderDirty();
		int startColumn = Math.Clamp(currentColumn, 0, Width - 1);
		int clampedWidth = Math.Max(0, Math.Min(width, Width - startColumn));
		int endColumn = startColumn + clampedWidth;
		for (int column = startColumn; column < endColumn; column++)
		{
			cells[currentRow, column] = new ScreenCell('\u2500', currentDisplayAttribute);
		}
		if (clampedWidth > 0)
		{
			currentColumn = Math.Min(Width - 1, endColumn - 1);
		}
		columnSlot = (ushort)currentColumn;
		RepaintRows(currentRow, currentRow);
		SyncLiveConsoleCursor();
	}

	private void ClearRegion(int topRow, int bottomRow)
	{
		for (int row = topRow; row <= bottomRow; row++)
		{
			ClearRow(row);
		}
	}

	private void ClearRow(int row)
	{
		for (int column = 0; column < Width; column++)
		{
			cells[row, column] = new ScreenCell(' ', currentDisplayAttribute);
		}
	}

	private void CopyRow(int sourceRow, int destinationRow)
	{
		for (int column = 0; column < Width; column++)
		{
			cells[destinationRow, column] = cells[sourceRow, column];
		}
	}

	/// <summary>
	/// Computes the CGA/EGA-style display attribute byte from the VM's style bitmask.
	/// Bit 0 = reverse video, bit 3 = bright, bit 5 = blink.
	/// </summary>
	private byte ComputeDisplayAttribute(ushort styleBits)
	{
		byte attribute = (styleBits & 0x0001) != 0 ? DefaultReverseBaseAttribute : normalBaseAttribute;
		if ((styleBits & 0x0008) != 0)
		{
			attribute |= BrightMaskAttribute;
		}

		if ((styleBits & 0x0020) != 0)
		{
			attribute |= BlinkMaskAttribute;
		}

		return attribute;
	}

	private void InitializeLiveConsole()
	{
		if (!liveConsoleEnabled)
		{
			return;
		}

		if (!OperatingSystem.IsWindows())
		{
			originalCursorVisible = true;
		}
		else
		{
		try
		{
			originalCursorVisible = Console.CursorVisible;
		}
		catch (IOException)
		{
			liveConsoleEnabled = false;
			return;
		}
		catch (PlatformNotSupportedException)
		{
			liveConsoleEnabled = false;
			return;
		}
		}

		TryEnsureConsoleSize();
		TryRefreshConsoleMetrics();
		TryClearConsole();
		TrySetCursorVisible(false);
		visibleCursorRow = currentRow;
		visibleCursorColumn = currentColumn;
		RepaintRows(0, Height - 1);
		SyncLiveConsoleCursor();
	}

	private void MoveCursor(int rowDelta, int columnDelta)
	{
		currentRow = Math.Clamp(currentRow + rowDelta, 0, Height - 1);
		currentColumn = Math.Clamp(currentColumn + columnDelta, 0, Width - 1);
		rowSlot = (ushort)currentRow;
		columnSlot = (ushort)currentColumn;
		if (inputCursorVisible)
		{
			SyncLiveConsoleCursor();
		}
	}

	private bool TryReadInteractiveKey()
	{
		if (!liveConsoleEnabled)
		{
			return false;
		}

		try
		{
			if (!Console.KeyAvailable)
			{
				return false;
			}

			QueueConsoleKey(Console.ReadKey(intercept: true));
			return inputQueue.Count > 0;
		}
		catch (InvalidOperationException)
		{
			return false;
		}
		catch (IOException)
		{
			return false;
		}
	}

	/// <summary>
	/// Translates a .NET <see cref="ConsoleKeyInfo"/> into one or more Cornerstone-style key values,
	/// using the DOS extended scan code convention (ESC prefix + scan code for special keys).
	/// </summary>
	private void QueueConsoleKey(ConsoleKeyInfo keyInfo)
	{
		if (keyInfo.Modifiers.HasFlag(ConsoleModifiers.Control) && keyInfo.Key == ConsoleKey.Backspace)
		{
			inputQueue.Enqueue(0x007F);
			return;
		}

		if (keyInfo.Modifiers.HasFlag(ConsoleModifiers.Shift) && keyInfo.Key == ConsoleKey.Tab)
		{
			inputQueue.Enqueue(0x001B);
			inputQueue.Enqueue(0x000F);
			return;
		}

		if (TryMapExtendedConsoleKey(keyInfo, out ushort mappedValue))
		{
			inputQueue.Enqueue(0x001B);
			inputQueue.Enqueue(mappedValue);
			return;
		}

		if (keyInfo.Key == ConsoleKey.Escape)
		{
			inputQueue.Enqueue(0x001B);
			inputQueue.Enqueue(0x001B);
			return;
		}

		if (keyInfo.KeyChar != '\0')
		{
			inputQueue.Enqueue((ushort)keyInfo.KeyChar);
		}
	}

	private static bool TryMapExtendedConsoleKey(ConsoleKeyInfo keyInfo, out ushort mappedValue)
	{
		if (keyInfo.Modifiers.HasFlag(ConsoleModifiers.Control) && ControlExtendedConsoleKeyMap.TryGetValue(keyInfo.Key, out mappedValue))
		{
			return true;
		}

		return ExtendedConsoleKeyMap.TryGetValue(keyInfo.Key, out mappedValue);
	}

	private void WriteConsoleCell(int row, int column, ScreenCell cell)
	{
		if (!liveConsoleEnabled)
		{
			return;
		}

		if ((consoleWidth <= 0 || consoleHeight <= 0) && !TryRefreshConsoleMetrics())
		{
			return;
		}

		if (row < 0 || column < 0 || row >= consoleHeight || column >= consoleWidth)
		{
			return;
		}

		if ((lastConsoleCursorColumn != column || lastConsoleCursorRow != row)
			&& !TrySetConsoleCursorPosition(column, row))
		{
			return;
		}

		ApplyConsoleColors(cell.Attribute);
		try
		{
			Console.Write(cell.Character);
			if (column + 1 < consoleWidth)
			{
				lastConsoleCursorColumn = column + 1;
				lastConsoleCursorRow = row;
			}
			else
			{
				lastConsoleCursorColumn = -1;
				lastConsoleCursorRow = -1;
			}
		}
		catch (IOException)
		{
		}
	}

	private void RepaintRows(int topRow, int bottomRow)
	{
		if (!liveConsoleEnabled)
		{
			return;
		}

		if (topRow > bottomRow)
		{
			(topRow, bottomRow) = (bottomRow, topRow);
		}

		int consoleWidth = Math.Min(Width, GetConsoleWidth());
		int consoleHeight = GetConsoleHeight();
		if (consoleWidth <= 0 || consoleHeight <= 0)
		{
			return;
		}

		for (int row = Math.Max(0, topRow); row <= Math.Min(bottomRow, Height - 1); row++)
		{
			if (row >= consoleHeight)
			{
				continue;
			}

			for (int column = 0; column < consoleWidth; column++)
			{
				WriteConsoleCell(row, column, cells[row, column]);
			}
		}
	}

	private void SyncLiveConsoleCursor()
	{
		if (!liveConsoleEnabled)
		{
			return;
		}

		if ((consoleWidth <= 0 || consoleHeight <= 0) && !TryRefreshConsoleMetrics())
		{
			return;
		}

		if (inputCursorVisible)
		{
			TrySetCursorVisible(true);
			TrySetConsoleCursorPosition(Math.Min(visibleCursorColumn, consoleWidth - 1), Math.Min(visibleCursorRow, consoleHeight - 1));
			return;
		}

		TrySetCursorVisible(false);
	}

	private void UpdateVisibleCursorFromCurrent()
	{
		visibleCursorRow = currentRow;
		visibleCursorColumn = currentColumn;
		inputCursorVisible = true;

		if (!liveConsoleEnabled)
		{
			return;
		}

		SyncLiveConsoleCursor();
	}

	private static ConsoleColor TryGetConsoleForegroundColor()
	{
		try
		{
			return Console.ForegroundColor;
		}
		catch (IOException)
		{
			return ConsoleColor.Gray;
		}
		catch (PlatformNotSupportedException)
		{
			return ConsoleColor.Gray;
		}
	}

	private static ConsoleColor TryGetConsoleBackgroundColor()
	{
		try
		{
			return Console.BackgroundColor;
		}
		catch (IOException)
		{
			return ConsoleColor.Black;
		}
		catch (PlatformNotSupportedException)
		{
			return ConsoleColor.Black;
		}
	}

	private void ApplyConsoleColors(byte attribute)
	{
		if (lastConsoleAttribute == attribute)
		{
			return;
		}

		ConsoleColor foreground = (ConsoleColor)(attribute & 0x0F);
		ConsoleColor background = (ConsoleColor)(((attribute >> 4) & 0x07) | ((attribute & 0x80) != 0 ? 0x08 : 0x00));
		TrySetConsoleColors(foreground, background);
		lastConsoleAttribute = attribute;
	}

	private static void TrySetConsoleColors(ConsoleColor foreground, ConsoleColor background)
	{
		try
		{
			if (Console.ForegroundColor != foreground)
			{
				Console.ForegroundColor = foreground;
			}

			if (Console.BackgroundColor != background)
			{
				Console.BackgroundColor = background;
			}
		}
		catch (IOException)
		{
		}
		catch (PlatformNotSupportedException)
		{
		}
	}

	private static int GetConsoleWidth()
	{
		try
		{
			return Console.BufferWidth;
		}
		catch (IOException)
		{
			return 0;
		}
		catch (PlatformNotSupportedException)
		{
			return 0;
		}
	}

	private static int GetConsoleHeight()
	{
		try
		{
			return Console.BufferHeight;
		}
		catch (IOException)
		{
			return 0;
		}
		catch (PlatformNotSupportedException)
		{
			return 0;
		}
	}

	private static void TryEnsureConsoleSize()
	{
		if (!OperatingSystem.IsWindows())
		{
			return;
		}

		try
		{
			int bufferWidth = Console.BufferWidth;
			int bufferHeight = Console.BufferHeight;
			int targetBufferWidth = Math.Max(bufferWidth, Math.Max(Width, Console.WindowWidth));
			int targetBufferHeight = Math.Max(bufferHeight, Math.Max(Height + 1, Console.WindowHeight));
			if (targetBufferWidth != bufferWidth || targetBufferHeight != bufferHeight)
			{
				Console.SetBufferSize(targetBufferWidth, targetBufferHeight);
			}

			int targetWindowWidth = Math.Min(Console.BufferWidth, Math.Max(Console.WindowWidth, Width));
			int targetWindowHeight = Math.Min(Console.BufferHeight, Math.Max(Console.WindowHeight, Height + 1));
			if (targetWindowWidth != Console.WindowWidth || targetWindowHeight != Console.WindowHeight)
			{
				Console.SetWindowSize(targetWindowWidth, targetWindowHeight);
			}
		}
		catch (ArgumentOutOfRangeException)
		{
		}
		catch (IOException)
		{
		}
		catch (PlatformNotSupportedException)
		{
		}
	}

	private static void TryClearConsole()
	{
		try
		{
			Console.Clear();
		}
		catch (IOException)
		{
		}
		catch (PlatformNotSupportedException)
		{
		}
	}

	private bool TryRefreshConsoleMetrics()
	{
		consoleWidth = GetConsoleWidth();
		consoleHeight = GetConsoleHeight();
		if (consoleWidth <= 0 || consoleHeight <= 0)
		{
			return false;
		}

		return true;
	}

	private bool TrySetConsoleCursorPosition(int column, int row)
	{
		if (column < 0 || row < 0)
		{
			return false;
		}

		try
		{
			Console.SetCursorPosition(column, row);
			lastConsoleCursorColumn = column;
			lastConsoleCursorRow = row;
			return true;
		}
		catch (ArgumentOutOfRangeException)
		{
			return false;
		}
		catch (IOException)
		{
			return false;
		}
		catch (PlatformNotSupportedException)
		{
			return false;
		}
	}

	private void TrySetCursorVisible(bool visible)
	{
		if (!OperatingSystem.IsWindows())
		{
			return;
		}

		if (lastCursorVisible == visible)
		{
			return;
		}

		try
		{
			Console.CursorVisible = visible;
			lastCursorVisible = visible;
		}
		catch (IOException)
		{
		}
		catch (PlatformNotSupportedException)
		{
		}
	}

	private void MarkRenderDirty()
	{
		renderCacheDirty = true;
	}
}
