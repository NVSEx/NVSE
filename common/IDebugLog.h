#pragma once

#include <cstdarg>

/**
 *	A simple debug log file
 *	
 *	This class supports prefix blocks describing the source of the log event.
 *	It also allows logical blocks and outlining.\n
 */
class IDebugLog
{
	public:
							IDebugLog();
							explicit IDebugLog(const char * name);
							~IDebugLog();

		static void			Open(const char * path);
		static void			OpenRelative(int folderID, const char * relPath);

		static void			Message(const char * message, const char * source = nullptr);
		static void			FormattedMessage(const char * fmt, ...);
		static void			FormattedMessage(const char * fmt, va_list args);

		enum LogLevel
		{
			kLevel_FatalError = 0,
			kLevel_Error,
			kLevel_Warning,
			kLevel_Message,
			kLevel_VerboseMessage,
			kLevel_DebugMessage
		};

		static void			Log(LogLevel level, const char * fmt, va_list args);

		static void			SetSource(const char * source);
		static void			ClearSource(void);

		static void			Indent(void);
		static void			Outdent(void);

		static void			OpenBlock(void);
		static void			CloseBlock(void);

		static void			SetAutoFlush(bool inAutoFlush);

		static void			SetLogLevel(LogLevel in)	{ logLevel = in; }
		static void			SetPrintLevel(LogLevel in)	{ printLevel = in; }

	private:
		static void			PrintSpaces(int numSpaces);
		static void			PrintText(const char * buf);
		static void			NewLine(void);

		static void			SeekCursor(int position);

		static int			TabSize(void);
		static int			RoundToTab(int spaces);

		static FILE			* logFile;			//!< the output file
		static char			cPath[_MAX_PATH];	//Path to the Logs Folder

		static char			sourceBuf[16];		//!< name of current source, used in prefix
		static char			headerText[16];		//!< current text to use as line prefix
		static char			formatBuf[8192];	//!< temp buffer used for formatted messages

		static int			indentLevel;		//!< the current indentation level (in tabs)
		static int			rightMargin;		//!< the column at which text should be wrapped
		static int			cursorPos;			//!< current cursor position
		static int			inBlock;			//!< are we in a block?

		static bool			autoFlush;			//!< automatically flush the file after writing

		static LogLevel		logLevel;			//!< least important log level to write
		static LogLevel		printLevel;			//!< least important log level to print
};

extern IDebugLog gLog;

inline void _FATALERROR(const char * fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	gLog.Log(IDebugLog::kLevel_FatalError, fmt, args);
	va_end(args);
}

inline void _ERROR(const char * fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	gLog.Log(IDebugLog::kLevel_Error, fmt, args);
	va_end(args);
}

inline void _WARNING(const char * fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	gLog.Log(IDebugLog::kLevel_Warning, fmt, args);
	va_end(args);
}

inline void _MESSAGE(const char * fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	gLog.Log(IDebugLog::kLevel_Message, fmt, args);
	va_end(args);
}

inline void _VMESSAGE(const char * fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	gLog.Log(IDebugLog::kLevel_VerboseMessage, fmt, args);
	va_end(args);
}

inline void _DMESSAGE(const char * fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	gLog.Log(IDebugLog::kLevel_DebugMessage, fmt, args);
	va_end(args);
}
