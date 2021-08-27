#include "common/IDebugLog.h"
#include <share.h>
#include "common/IFileStream.h"
#include <shlobj.h>

std::FILE			* IDebugLog::logFile = nullptr;
char				IDebugLog::cPath[MAX_PATH] = { 0 };
char				IDebugLog::sourceBuf[16] = { 0 };
char				IDebugLog::headerText[16] = { 0 };
char				IDebugLog::formatBuf[8192] = { 0 };
int					IDebugLog::indentLevel = 0;
int					IDebugLog::rightMargin = 0;
int					IDebugLog::cursorPos = 0;
int					IDebugLog::inBlock = 0;
bool				IDebugLog::autoFlush = true;
IDebugLog::LogLevel	IDebugLog::logLevel = IDebugLog::kLevel_DebugMessage;
IDebugLog::LogLevel	IDebugLog::printLevel = IDebugLog::kLevel_Message;

IDebugLog::IDebugLog() = default;

IDebugLog::IDebugLog(const char * name)
{
	GetCurrentDirectoryA(MAX_PATH, IDebugLog::cPath); // Find CWD (e.g. C:/Steam/SteamApps/Common/Fallout New Vegas)
	Open(name);
}

IDebugLog::~IDebugLog()
{
	if(logFile)
		fclose(logFile);
}

void IDebugLog::Open(const char * path)
{
	char name[1024] = { 0 };
	sprintf_s(name, sizeof(name), "%s\\%s\\%s", IDebugLog::cPath, "Logs", path); // NV/Logs/File
	logFile = _fsopen(name, "w", _SH_DENYWR);
	if(!logFile) {
		UInt32 id = 0;
		do
		{
			// On first failure, create folder.
			if (id == 0) {
				char cLogDir[1024] = { 0 };
				sprintf_s(cLogDir, sizeof(cLogDir), "%s\\%s", IDebugLog::cPath, "Logs"); // NV/Logs
				if (!(CreateDirectoryA(cLogDir, nullptr) || ERROR_ALREADY_EXISTS == GetLastError()))
				{
					return; // Failed to create directory.
				}
				// Try to create file
				sprintf_s(name, sizeof(name), "%s\\%s\\%s", IDebugLog::cPath, "Logs", path);
				id++;
				logFile = nullptr;
				logFile = _fsopen(name, "w", _SH_DENYWR);
			}
			else {
				// Cannot open file, attempt again with file+ID.
				sprintf_s(name, sizeof(name), "%s\\%s\\%s%d", IDebugLog::cPath, "Logs", path, id); //File Not Open?
				id++;

				logFile = nullptr;
				logFile = _fsopen(name, "w", _SH_DENYWR);
			}
		}
		while(!logFile && (id < 5));
	}
}

void IDebugLog::OpenRelative(int folderID, const char * relPath)
{
	char	path[MAX_PATH];

	ASSERT(SUCCEEDED(SHGetFolderPath(NULL, folderID, NULL, SHGFP_TYPE_CURRENT, path)));

	strcat_s(path, sizeof(path), relPath);

	IFileStream::MakeAllDirs(path);

	Open(path);
}

/**
 *	Output a non-formatted message to the log file
 *	
 *	@param message the message
 *	@param source the source of the message, or NULL to use the previous source
 */
void IDebugLog::Message(const char * message, const char * source)
{
	if(source)
		SetSource(source);

	if(inBlock)
	{
		SeekCursor(RoundToTab((indentLevel * 4) + strlen(headerText)));
	}
	else
	{
		SeekCursor(indentLevel * 4);

		PrintText(headerText);
	}

	PrintText(message);
	NewLine();
}

/**
 *	Output a formatted message to the log file
 *	
 *	@note It is impossible to set the source of a formatted message.
 *	The previous source will be used.
 */
void IDebugLog::FormattedMessage(const char * fmt, ...)
{
	va_list	argList;

	va_start(argList, fmt);
	vsprintf_s(formatBuf, sizeof(formatBuf), fmt, argList);
	Message(formatBuf);
	va_end(argList);
}

/**
 *	Output a formatted message to the log file
 *	
 *	@note It is impossible to set the source of a formatted message.
 *	The previous source will be used.
 */
void IDebugLog::FormattedMessage(const char * fmt, va_list args)
{
	vsprintf_s(formatBuf, sizeof(formatBuf), fmt, args);
	Message(formatBuf);
}

void IDebugLog::Log(LogLevel level, const char * fmt, va_list args)
{
	bool	log = (level <= logLevel);
	bool	print = (level <= printLevel);

	if(log || print)
		vsprintf_s(formatBuf, sizeof(formatBuf), fmt, args);

	if(log)
		Message(formatBuf);
	
	if(print)
		printf("%s\n", formatBuf);
}

/**
 *	Set the current message source
 */
void IDebugLog::SetSource(const char * source)
{
	strcpy_s(sourceBuf, sizeof(sourceBuf), source);
	strcpy_s(headerText, sizeof(headerText), "[        ]\t");
	
	char	* tgt = headerText + 1;
	char	* src = sourceBuf;

	for(int i = 0; (i < 8) && *src; i++, tgt++, src++)
		*tgt = *src;
}

/**
 *	Clear the current message source
 */
void IDebugLog::ClearSource(void)
{
	sourceBuf[0] = 0;
}

/**
 *	Increase the indentation level
 */
void IDebugLog::Indent(void)
{
	indentLevel++;
}

/**
 *	Decrease the indentation level
 */
void IDebugLog::Outdent(void)
{
	if(indentLevel)
		indentLevel--;
}

/**
 *	Enter a logical block
 */
void IDebugLog::OpenBlock(void)
{
	SeekCursor(indentLevel * 4);

	PrintText(headerText);

	inBlock = 1;
}

/**
 *	Close a logical block
 */
void IDebugLog::CloseBlock(void)
{
	inBlock = 0;
}

/**
 *	Enable/disable autoflush
 *	
 *	@param inAutoFlush autoflush state
 */
void IDebugLog::SetAutoFlush(bool inAutoFlush)
{
	autoFlush = inAutoFlush;
}

/**
 *	Print spaces to the log
 *	
 *	If possible, tabs are used instead of spaces.
 */
void IDebugLog::PrintSpaces(int numSpaces)
{
	int	originalNumSpaces = numSpaces;

	if(logFile)
	{
		while(numSpaces > 0)
		{
			if(numSpaces >= TabSize())
			{
				numSpaces -= TabSize();
				fputc('\t', logFile);
			}
			else
			{
				numSpaces--;
				fputc(' ', logFile);
			}
		}
	}

	cursorPos += originalNumSpaces;
}

/**
 *	Prints raw text to the log file
 */
void IDebugLog::PrintText(const char * buf)
{
	if(logFile)
	{
		fputs(buf, logFile);
		if(autoFlush)
			fflush(logFile);
	}

	const char	* traverse = buf;
	char		data;

	while(data = *traverse++)
	{
		if(data == '\t')
			cursorPos += TabSize();
		else
			cursorPos++;
	}
}

/**
 *	Moves to the next line of the log file
 */
void IDebugLog::NewLine(void)
{
	if(logFile)
	{
		fputc('\n', logFile);

		if(autoFlush)
			fflush(logFile);
	}

	cursorPos = 0;
}

/**
 *	Prints spaces to align the cursor to the requested position
 *	
 *	@note The cursor move will not be performed if the request would move the cursor
 *	backwards.
 */
void IDebugLog::SeekCursor(int position)
{
	if(position > cursorPos)
		PrintSpaces(position - cursorPos);
}

/**
 *	Returns the number of spaces a tab would occupy at the current cursor position
 */
int IDebugLog::TabSize(void)
{
	return ((~cursorPos) & 3) + 1;
}

/**
 *	Rounds a number of spaces to the nearest tab
 */
int IDebugLog::RoundToTab(int spaces)
{
	return (spaces + 3) & ~3;
}
