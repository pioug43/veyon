/*
 * ExamModeVdiClient.cpp - cross-platform VDI client checks (Omnissa Horizon)
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

#include "ExamModeVdiClient.h"

#include <QFile>

#if defined(Q_OS_WIN)
#include <string>
#include <qt_windows.h>
#include <wtsapi32.h>
#elif defined(Q_OS_LINUX) && defined(EXAMMODE_HAVE_X11)
#include <QProcess>
#include <cstring>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#elif defined(Q_OS_LINUX)
#include <QProcess>
#endif

namespace ExamModeVdiClient
{

namespace
{

// Le nom d'exécutable correspond-il au client VDI Horizon ? Robuste au renommage
// VMware→Omnissa et aux variantes de version. (Inutilisé sur les plateformes sans
// implémentation dédiée — d'où [[maybe_unused]].)
[[maybe_unused]] bool imageMatchesVdiClient( const QString& imageBasenameLower )
{
	return imageBasenameLower.contains( QLatin1String("vmware-view") ) ||
		   imageBasenameLower.contains( QLatin1String("omnissa") ) ||
		   ( imageBasenameLower.contains( QLatin1String("horizon") ) &&
			 imageBasenameLower.contains( QLatin1String("client") ) );
}

}


// ==========================================================================
#if defined(Q_OS_WIN)
// ==========================================================================

namespace
{

bool windowBelongsToVdiClient( HWND hwnd )
{
	DWORD pid = 0;
	GetWindowThreadProcessId( hwnd, &pid );
	if( pid == 0 )
	{
		return false;
	}
	const HANDLE process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid );
	if( process == nullptr )
	{
		return false;
	}
	wchar_t buffer[MAX_PATH];
	DWORD size = MAX_PATH;
	QString base;
	if( QueryFullProcessImageNameW( process, 0, buffer, &size ) )
	{
		const auto full = QString::fromWCharArray( buffer, static_cast<int>( size ) );
		const auto separator = qMax( full.lastIndexOf( QLatin1Char('\\') ), full.lastIndexOf( QLatin1Char('/') ) );
		base = full.mid( separator + 1 ).toLower();
	}
	CloseHandle( process );
	return imageMatchesVdiClient( base );
}


struct Scan
{
	bool found{false};
	bool fullscreen{false};
};


BOOL CALLBACK scanProc( HWND hwnd, LPARAM lparam )
{
	if( IsWindowVisible( hwnd ) == FALSE || windowBelongsToVdiClient( hwnd ) == false )
	{
		return TRUE;
	}
	RECT windowRect;
	if( GetWindowRect( hwnd, &windowRect ) == FALSE )
	{
		return TRUE;
	}
	if( ( windowRect.right - windowRect.left ) < 200 || ( windowRect.bottom - windowRect.top ) < 200 )
	{
		return TRUE;	// fenêtres auxiliaires (barres d'outils, notifications)
	}
	MONITORINFO monitorInfo;
	monitorInfo.cbSize = sizeof( monitorInfo );
	if( GetMonitorInfoW( MonitorFromWindow( hwnd, MONITOR_DEFAULTTONEAREST ), &monitorInfo ) == FALSE )
	{
		return TRUE;
	}
	auto* scan = reinterpret_cast<Scan*>( lparam );
	scan->found = true;
	const RECT& m = monitorInfo.rcMonitor;	// moniteur entier (barre des tâches incluse)
	if( windowRect.left <= m.left && windowRect.top <= m.top &&
		windowRect.right >= m.right && windowRect.bottom >= m.bottom )
	{
		scan->fullscreen = true;
		return FALSE;
	}
	return TRUE;
}


struct Pick
{
	HWND hwnd{nullptr};
	long long area{0};
};


BOOL CALLBACK pickProc( HWND hwnd, LPARAM lparam )
{
	if( IsWindowVisible( hwnd ) == FALSE || windowBelongsToVdiClient( hwnd ) == false )
	{
		return TRUE;
	}
	RECT rect;
	if( GetWindowRect( hwnd, &rect ) == FALSE )
	{
		return TRUE;
	}
	const long long width = rect.right - rect.left;
	const long long height = rect.bottom - rect.top;
	if( width < 200 || height < 200 )
	{
		return TRUE;
	}
	auto* pick = reinterpret_cast<Pick*>( lparam );
	if( width * height > pick->area )
	{
		pick->area = width * height;
		pick->hwnd = hwnd;
	}
	return TRUE;
}

}


State fullscreenState()
{
	Scan scan;
	EnumWindows( scanProc, reinterpret_cast<LPARAM>( &scan ) );
	if( scan.found == false )
	{
		return State::Unknown;
	}
	return scan.fullscreen ? State::Fullscreen : State::NotFullscreen;
}


bool forceFullscreen()
{
	Pick pick;
	EnumWindows( pickProc, reinterpret_cast<LPARAM>( &pick ) );
	if( pick.hwnd == nullptr )
	{
		return false;
	}
	MONITORINFO monitorInfo;
	monitorInfo.cbSize = sizeof( monitorInfo );
	if( GetMonitorInfoW( MonitorFromWindow( pick.hwnd, MONITOR_DEFAULTTONEAREST ), &monitorInfo ) == FALSE )
	{
		return false;
	}
	const RECT& m = monitorInfo.rcMonitor;
	ShowWindow( pick.hwnd, SW_RESTORE );
	SetForegroundWindow( pick.hwnd );
	SetWindowPos( pick.hwnd, HWND_TOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top,
		SWP_NOOWNERZORDER | SWP_FRAMECHANGED );
	RECT windowRect;
	if( GetWindowRect( pick.hwnd, &windowRect ) == FALSE )
	{
		return false;
	}
	return windowRect.left <= m.left && windowRect.top <= m.top &&
		   windowRect.right >= m.right && windowRect.bottom >= m.bottom;
}


void showSessionMessage( const QString& title, const QString& text, int timeoutSeconds )
{
	DWORD sessionId = WTSGetActiveConsoleSessionId();
	if( sessionId == 0xFFFFFFFFu )
	{
		sessionId = WTS_CURRENT_SESSION;
	}
	auto titleUtf16 = title.toStdWString();
	auto textUtf16 = text.toStdWString();
	DWORD response = 0;
	// bWait=FALSE : non bloquant ; la boîte se ferme seule après timeoutSeconds.
	WTSSendMessageW( WTS_CURRENT_SERVER_HANDLE, sessionId,
		titleUtf16.data(), static_cast<DWORD>( titleUtf16.size() * sizeof( wchar_t ) ),
		textUtf16.data(), static_cast<DWORD>( textUtf16.size() * sizeof( wchar_t ) ),
		MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST,
		timeoutSeconds, &response, FALSE );
}


// ==========================================================================
#elif defined(Q_OS_LINUX) && defined(EXAMMODE_HAVE_X11)
// ==========================================================================

namespace
{

Display* openDisplay()
{
	// Respecte $DISPLAY (composant en session graphique) ; repli sur :0.
	Display* display = XOpenDisplay( nullptr );
	if( display == nullptr )
	{
		display = XOpenDisplay( ":0" );
	}
	return display;
}


// Liste des fenêtres gérées (_NET_CLIENT_LIST, EWMH). L'appelant libère via XFree.
Window* clientList( Display* display, Window root, unsigned long* count )
{
	*count = 0;
	const Atom property = XInternAtom( display, "_NET_CLIENT_LIST", True );
	if( property == None )
	{
		return nullptr;
	}
	Atom actualType = None;
	int actualFormat = 0;
	unsigned long bytesAfter = 0;
	unsigned char* data = nullptr;
	if( XGetWindowProperty( display, root, property, 0, 4096, False, XA_WINDOW,
			&actualType, &actualFormat, count, &bytesAfter, &data ) != Success )
	{
		return nullptr;
	}
	return reinterpret_cast<Window*>( data );
}


long windowPid( Display* display, Window window )
{
	const Atom property = XInternAtom( display, "_NET_WM_PID", True );
	if( property == None )
	{
		return 0;
	}
	Atom actualType = None;
	int actualFormat = 0;
	unsigned long items = 0;
	unsigned long bytesAfter = 0;
	unsigned char* data = nullptr;
	if( XGetWindowProperty( display, window, property, 0, 1, False, XA_CARDINAL,
			&actualType, &actualFormat, &items, &bytesAfter, &data ) != Success || data == nullptr )
	{
		return 0;
	}
	const long pid = items > 0 ? static_cast<long>( *reinterpret_cast<unsigned long*>( data ) ) : 0;
	XFree( data );
	return pid;
}


bool pidMatchesVdiClient( long pid )
{
	if( pid <= 0 )
	{
		return false;
	}
	// comm (nom court) puis cmdline (arg0) pour couvrir les binaires enveloppés.
	for( const auto& node : { QStringLiteral("/proc/%1/comm"), QStringLiteral("/proc/%1/cmdline") } )
	{
		QFile file( node.arg( pid ) );
		if( file.open( QIODevice::ReadOnly ) == false )
		{
			continue;
		}
		auto content = QString::fromLocal8Bit( file.readAll() );
		file.close();
		// cmdline : arguments séparés par des NUL → on ne garde que arg0.
		content = content.section( QChar::Null, 0, 0 ).trimmed();
		const auto separator = qMax( content.lastIndexOf( QLatin1Char('/') ), -1 );
		const auto base = content.mid( separator + 1 ).toLower();
		if( imageMatchesVdiClient( base ) )
		{
			return true;
		}
	}
	return false;
}


bool windowIsFullscreen( Display* display, Window window, int screenWidth, int screenHeight )
{
	// Signal EWMH prioritaire : _NET_WM_STATE contient _NET_WM_STATE_FULLSCREEN.
	const Atom stateProperty = XInternAtom( display, "_NET_WM_STATE", True );
	const Atom fullscreenAtom = XInternAtom( display, "_NET_WM_STATE_FULLSCREEN", True );
	if( stateProperty != None && fullscreenAtom != None )
	{
		Atom actualType = None;
		int actualFormat = 0;
		unsigned long items = 0;
		unsigned long bytesAfter = 0;
		unsigned char* data = nullptr;
		if( XGetWindowProperty( display, window, stateProperty, 0, 32, False, XA_ATOM,
				&actualType, &actualFormat, &items, &bytesAfter, &data ) == Success && data != nullptr )
		{
			const auto* atoms = reinterpret_cast<Atom*>( data );
			bool fullscreen = false;
			for( unsigned long i = 0; i < items; ++i )
			{
				if( atoms[i] == fullscreenAtom )
				{
					fullscreen = true;
					break;
				}
			}
			XFree( data );
			if( fullscreen )
			{
				return true;
			}
		}
	}
	// Repli géométrique : la fenêtre couvre-t-elle tout l'écran ?
	XWindowAttributes attributes;
	if( XGetWindowAttributes( display, window, &attributes ) == 0 )
	{
		return false;
	}
	int x = 0;
	int y = 0;
	Window child = 0;
	XTranslateCoordinates( display, window, attributes.root, 0, 0, &x, &y, &child );
	return x <= 0 && y <= 0 && ( x + attributes.width ) >= screenWidth &&
		   ( y + attributes.height ) >= screenHeight;
}

}


State fullscreenState()
{
	Display* display = openDisplay();
	if( display == nullptr )
	{
		return State::Unknown;
	}
	const int screen = DefaultScreen( display );
	const Window root = RootWindow( display, screen );
	const int screenWidth = DisplayWidth( display, screen );
	const int screenHeight = DisplayHeight( display, screen );

	unsigned long count = 0;
	Window* windows = clientList( display, root, &count );
	State result = State::Unknown;
	if( windows != nullptr )
	{
		for( unsigned long i = 0; i < count; ++i )
		{
			if( pidMatchesVdiClient( windowPid( display, windows[i] ) ) == false )
			{
				continue;
			}
			result = windowIsFullscreen( display, windows[i], screenWidth, screenHeight ) ?
				State::Fullscreen : State::NotFullscreen;
			if( result == State::Fullscreen )
			{
				break;
			}
		}
		XFree( windows );
	}
	XCloseDisplay( display );
	return result;
}


bool forceFullscreen()
{
	Display* display = openDisplay();
	if( display == nullptr )
	{
		return false;
	}
	const int screen = DefaultScreen( display );
	const Window root = RootWindow( display, screen );
	const int screenWidth = DisplayWidth( display, screen );
	const int screenHeight = DisplayHeight( display, screen );

	// Sélectionne la plus grande fenêtre du client.
	unsigned long count = 0;
	Window* windows = clientList( display, root, &count );
	Window target = 0;
	long long bestArea = 0;
	if( windows != nullptr )
	{
		for( unsigned long i = 0; i < count; ++i )
		{
			if( pidMatchesVdiClient( windowPid( display, windows[i] ) ) == false )
			{
				continue;
			}
			XWindowAttributes attributes;
			if( XGetWindowAttributes( display, windows[i], &attributes ) == 0 )
			{
				continue;
			}
			const long long area = static_cast<long long>( attributes.width ) * attributes.height;
			if( area > bestArea )
			{
				bestArea = area;
				target = windows[i];
			}
		}
		XFree( windows );
	}
	if( target == 0 )
	{
		XCloseDisplay( display );
		return false;
	}

	// Requête EWMH : activer + passer en plein écran (le WM applique).
	const Atom activeProperty = XInternAtom( display, "_NET_ACTIVE_WINDOW", True );
	const Atom stateProperty = XInternAtom( display, "_NET_WM_STATE", True );
	const Atom fullscreenAtom = XInternAtom( display, "_NET_WM_STATE_FULLSCREEN", True );
	if( stateProperty != None && fullscreenAtom != None )
	{
		constexpr long NetWmStateAdd = 1;
		XEvent event;
		memset( &event, 0, sizeof( event ) );
		event.xclient.type = ClientMessage;
		event.xclient.window = target;
		event.xclient.message_type = stateProperty;
		event.xclient.format = 32;
		event.xclient.data.l[0] = NetWmStateAdd;
		event.xclient.data.l[1] = static_cast<long>( fullscreenAtom );
		event.xclient.data.l[2] = 0;
		event.xclient.data.l[3] = 1;	// source : application
		XSendEvent( display, root, False, SubstructureNotifyMask | SubstructureRedirectMask, &event );
	}
	if( activeProperty != None )
	{
		XEvent event;
		memset( &event, 0, sizeof( event ) );
		event.xclient.type = ClientMessage;
		event.xclient.window = target;
		event.xclient.message_type = activeProperty;
		event.xclient.format = 32;
		event.xclient.data.l[0] = 1;
		XSendEvent( display, root, False, SubstructureNotifyMask | SubstructureRedirectMask, &event );
	}
	XSync( display, False );
	usleep( 250 * 1000 );	// laisser le WM appliquer avant la revérification

	const bool fullscreen = windowIsFullscreen( display, target, screenWidth, screenHeight );
	XCloseDisplay( display );
	return fullscreen;
}


void showSessionMessage( const QString& title, const QString& text, int timeoutSeconds )
{
	// notify-send (best-effort) : nécessite le bus de session utilisateur.
	QProcess::startDetached( QStringLiteral("notify-send"), {
		QStringLiteral("-u"), QStringLiteral("critical"),
		QStringLiteral("-t"), QString::number( timeoutSeconds * 1000 ),
		title, text } );
}


// ==========================================================================
#else	// autres plateformes / Linux sans X11
// ==========================================================================

State fullscreenState()
{
	return State::Unknown;
}

bool forceFullscreen()
{
	return false;
}

void showSessionMessage( const QString& title, const QString& text, int timeoutSeconds )
{
#if defined(Q_OS_LINUX)
	// X11 absent au build : tenter tout de même une notification best-effort.
	QProcess::startDetached( QStringLiteral("notify-send"), {
		QStringLiteral("-u"), QStringLiteral("critical"),
		QStringLiteral("-t"), QString::number( timeoutSeconds * 1000 ),
		title, text } );
#else
	Q_UNUSED( title )
	Q_UNUSED( text )
	Q_UNUSED( timeoutSeconds )
#endif
}

#endif

}
