/*
 * CrashHandler.cpp - implementation of the cross-platform crash capture
 *
 * Copyright (c) 2026 Pierrick Belledent
 *
 * This file is part of Veyon - https://veyon.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>

#include "CrashHandler.h"
#include "HostAddress.h"
#include "VeyonCore.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#else
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <execinfo.h>
#endif

namespace
{

// Contexte pré-calculé au moment de l'installation : on évite d'avoir à
// dériver ces informations dans le gestionnaire de crash (contexte fragile).
struct Context
{
	QString spoolDir;
	QString component;
	QString version;
	QString host;
	QString os;
	QString kernel;
	QString cpuArch;
	QString buildAbi;
	QString logDirectory;
	qint64 pid = 0;
	bool installed = false;
};

Context g_ctx;

constexpr int LogTailMaxBytes = 64 * 1024;
constexpr int BackTraceMaxDepth = 64;

QString reportBasePath()
{
	const auto ts = QDateTime::currentDateTime().toString( QStringLiteral("yyyyMMdd-HHmmss-zzz") );
	return QDir( g_ctx.spoolDir ).absoluteFilePath(
		QStringLiteral("crash-%1-%2-%3").arg( g_ctx.component, ts ).arg( g_ctx.pid ) );
}

// Queue du journal Veyon le plus récemment écrit (celui de ce processus) —
// jointe au rapport pour donner le contexte juste avant le crash.
QString logTail()
{
	if( g_ctx.logDirectory.isEmpty() )
	{
		return {};
	}

	const auto entries = QDir( g_ctx.logDirectory ).entryInfoList(
		{ QStringLiteral("Veyon*.log") }, QDir::Files, QDir::Time );
	if( entries.isEmpty() )
	{
		return {};
	}

	QFile f( entries.first().absoluteFilePath() );
	if( f.open( QIODevice::ReadOnly | QIODevice::Text ) == false )
	{
		return {};
	}

	const auto size = f.size();
	if( size > LogTailMaxBytes )
	{
		f.seek( size - LogTailMaxBytes );
	}
	return QString::fromUtf8( f.readAll() );
}

// Écrit le rapport de métadonnées JSON — riche par conception pour maximiser
// le diagnostic ultérieur.
void writeReport( const QString& basePath, const QString& reason, const QString& details,
				  const QStringList& backtrace, const QString& dumpFileName )
{
	QJsonObject o;
	o.insert( QStringLiteral("schema"), 1 );
	o.insert( QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString( Qt::ISODate ) );
	o.insert( QStringLiteral("component"), g_ctx.component );
	o.insert( QStringLiteral("application"), QCoreApplication::applicationName() );
	o.insert( QStringLiteral("veyonVersion"), g_ctx.version );
	o.insert( QStringLiteral("host"), g_ctx.host );
	o.insert( QStringLiteral("pid"), g_ctx.pid );
	o.insert( QStringLiteral("os"), g_ctx.os );
	o.insert( QStringLiteral("kernel"), g_ctx.kernel );
	o.insert( QStringLiteral("cpuArch"), g_ctx.cpuArch );
	o.insert( QStringLiteral("buildAbi"), g_ctx.buildAbi );
	o.insert( QStringLiteral("reason"), reason );
	o.insert( QStringLiteral("details"), details );
	if( backtrace.isEmpty() == false )
	{
		o.insert( QStringLiteral("backtrace"), QJsonArray::fromStringList( backtrace ) );
	}
	if( dumpFileName.isEmpty() == false )
	{
		o.insert( QStringLiteral("minidump"), dumpFileName );
	}
	o.insert( QStringLiteral("logTail"), logTail() );

	QFile f( basePath + QStringLiteral(".json") );
	if( f.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
	{
		f.write( QJsonDocument( o ).toJson( QJsonDocument::Indented ) );
		f.close();
	}
}


#ifdef Q_OS_WIN

// Filtre d'exception de dernier recours : écrit un minidump complet puis les
// métadonnées, et termine le processus proprement.
LONG WINAPI topLevelExceptionFilter( EXCEPTION_POINTERS* info )
{
	const auto base = reportBasePath();

	QString dumpFileName;
	const QString dumpPath = base + QStringLiteral(".dmp");
	HANDLE hFile = CreateFileW( reinterpret_cast<const wchar_t *>( dumpPath.utf16() ),
								GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
	if( hFile != INVALID_HANDLE_VALUE )
	{
		MINIDUMP_EXCEPTION_INFORMATION mei;
		mei.ThreadId = GetCurrentThreadId();
		mei.ExceptionPointers = info;
		mei.ClientPointers = FALSE;

		const auto type = static_cast<MINIDUMP_TYPE>(
			MiniDumpWithDataSegs | MiniDumpWithHandleData |
			MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData );

		if( MiniDumpWriteDump( GetCurrentProcess(), GetCurrentProcessId(), hFile,
							   type, &mei, nullptr, nullptr ) )
		{
			dumpFileName = QFileInfo( dumpPath ).fileName();
		}
		CloseHandle( hFile );
	}

	const auto* record = info->ExceptionRecord;
	const QString details =
		QStringLiteral("exceptionCode=0x%1 faultAddress=0x%2 threadId=%3 flags=0x%4")
			.arg( static_cast<quint32>( record->ExceptionCode ), 8, 16, QLatin1Char('0') )
			.arg( reinterpret_cast<quintptr>( record->ExceptionAddress ), 0, 16 )
			.arg( GetCurrentThreadId() )
			.arg( static_cast<quint32>( record->ExceptionFlags ), 0, 16 );

	writeReport( base, QStringLiteral("unhandled-exception"), details, {}, dumpFileName );

	// Exception considérée comme traitée au niveau supérieur → le processus se
	// termine (pas de dialogue WER, notre dump est déjà écrit).
	return EXCEPTION_EXECUTE_HANDLER;
}

#else

// Anciens gestionnaires (p.ex. celui du plugin plateforme Linux) — on les
// chaîne après avoir écrit le rapport, pour préserver leur comportement.
struct sigaction g_oldActions[NSIG];

void signalHandler( int sig, siginfo_t* siginfo, void* ucontext )
{
	Q_UNUSED(ucontext)

	const auto base = reportBasePath();

	void* frames[BackTraceMaxDepth];
	const int frameCount = backtrace( frames, BackTraceMaxDepth );

	// 1) Trace brute via un fd (chemin le plus robuste, écrit en premier).
	const QString tracePathStr = base + QStringLiteral(".trace");
	const auto tracePath = tracePathStr.toLocal8Bit();
	const int fd = ::open( tracePath.constData(), O_WRONLY | O_CREAT | O_TRUNC, 0600 );
	if( fd >= 0 )
	{
		backtrace_symbols_fd( frames, frameCount, fd );
		::close( fd );
	}

	// 2) Backtrace lisible pour le rapport JSON.
	QStringList bt;
	char** symbols = backtrace_symbols( frames, frameCount );
	if( symbols )
	{
		bt.reserve( frameCount );
		for( int i = 0; i < frameCount; ++i )
		{
			bt.append( QString::fromLocal8Bit( symbols[i] ) );
		}
		free( symbols );
	}

	QString details = QStringLiteral("signal=%1 (%2)").arg( sig ).arg(
		QString::fromLocal8Bit( ::strsignal( sig ) ) );
	if( siginfo )
	{
		details += QStringLiteral(" code=%1 faultAddress=0x%2")
			.arg( siginfo->si_code )
			.arg( reinterpret_cast<quintptr>( siginfo->si_addr ), 0, 16 );
	}

	writeReport( base, QStringLiteral("signal"), details, bt, {} );

	// Chaîner l'ancien gestionnaire s'il existe, sinon rétablir le comportement
	// par défaut et relancer le signal (génération d'un core dump / terminaison).
	if( sig > 0 && sig < NSIG )
	{
		const auto& old = g_oldActions[sig];
		if( ( old.sa_flags & SA_SIGINFO ) && old.sa_sigaction )
		{
			old.sa_sigaction( sig, siginfo, ucontext );
			return;
		}
		if( old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN && old.sa_handler )
		{
			old.sa_handler( sig );
			return;
		}
	}

	::signal( sig, SIG_DFL );
	::raise( sig );
}

#endif

} // namespace



void CrashHandler::install( const QString& spoolDir, const QString& componentName, const QString& logDirectory )
{
	if( g_ctx.installed )
	{
		return;
	}

	g_ctx.spoolDir = spoolDir;
	g_ctx.component = componentName;
	g_ctx.version = VeyonCore::versionString();
	g_ctx.host = HostAddress::localFQDN();
	g_ctx.os = QStringLiteral("%1 %2 %3").arg( QSysInfo::prettyProductName(),
											   QSysInfo::productType(), QSysInfo::productVersion() );
	g_ctx.kernel = QStringLiteral("%1 %2").arg( QSysInfo::kernelType(), QSysInfo::kernelVersion() );
	g_ctx.cpuArch = QSysInfo::currentCpuArchitecture();
	g_ctx.buildAbi = QSysInfo::buildAbi();
	g_ctx.logDirectory = logDirectory;
	g_ctx.pid = QCoreApplication::applicationPid();

	QDir().mkpath( spoolDir );

#ifdef Q_OS_WIN
	SetUnhandledExceptionFilter( topLevelExceptionFilter );
#else
	struct sigaction sa;
	memset( &sa, 0, sizeof( sa ) );
	sa.sa_sigaction = signalHandler;
	sigemptyset( &sa.sa_mask );
	sa.sa_flags = SA_SIGINFO | SA_RESTART;

	for( const int sig : { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL } )
	{
		::sigaction( sig, &sa, &g_oldActions[sig] );
	}
#endif

	g_ctx.installed = true;
}
