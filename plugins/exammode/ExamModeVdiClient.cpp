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
#include <QStringList>
#include <QVector>

#if defined(Q_OS_WIN)
#include <string>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <qt_windows.h>
#include <wtsapi32.h>
#elif defined(Q_OS_LINUX) && defined(EXAMMODE_HAVE_X11)
#include <QDir>
#include <QProcess>
#include <cstring>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#if defined(EXAMMODE_HAVE_XRANDR)
#include <X11/extensions/Xrandr.h>
#endif
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


// --- Topologies d'écrans (partagé Windows/Linux, mode invité VDI) ----------
// L'agent Horizon publie les écrans PHYSIQUES du poste étudiant tels que
// rapportés par le client (« {largeur,hauteur,gauche,haut,bpp,primaire},… ») ;
// chaque plateforme fournit ensuite la topologie COURANTE réellement affichée
// par la session distante. Couverture partielle = une portion de l'hôte
// physique reste visible/accessible → NotFullscreen.

struct Rect
{
	long left{0};
	long top{0};
	long width{0};
	long height{0};
};


// Parse « {w,h,x,y,bpp,primaire},{...} » (ViewClient_Displays.Topology).
[[maybe_unused]] QVector<Rect> parsePhysicalTopology( const QString& value )
{
	QVector<Rect> rects;
	int i = 0;
	while( ( i = value.indexOf( QLatin1Char('{'), i ) ) >= 0 )
	{
		const int end = value.indexOf( QLatin1Char('}'), i );
		if( end < 0 )
		{
			break;
		}
		const auto fields = value.mid( i + 1, end - i - 1 ).split( QLatin1Char(',') );
		if( fields.size() >= 4 )
		{
			Rect r;
			r.width  = fields[0].trimmed().toLong();
			r.height = fields[1].trimmed().toLong();
			r.left   = fields[2].trimmed().toLong();
			r.top    = fields[3].trimmed().toLong();
			if( r.width > 0 && r.height > 0 )
			{
				rects.append( r );
			}
		}
		i = end + 1;
	}
	return rects;
}


// Étendue (boîte englobante) d'un ensemble de rectangles.
[[maybe_unused]] void boundingBox( const QVector<Rect>& rects, long& width, long& height )
{
	width = 0;
	height = 0;
	if( rects.isEmpty() )
	{
		return;
	}
	long minX = rects.first().left;
	long minY = rects.first().top;
	long maxX = rects.first().left + rects.first().width;
	long maxY = rects.first().top + rects.first().height;
	for( const auto& r : rects )
	{
		minX = qMin( minX, r.left );
		minY = qMin( minY, r.top );
		maxX = qMax( maxX, r.left + r.width );
		maxY = qMax( maxY, r.top + r.height );
	}
	width = maxX - minX;
	height = maxY - minY;
}


// Verdict plein écran : la session distante (current) couvre-t-elle tout le
// matériel physique du poste étudiant (physical) ? trustCurrentCount=false
// quand « current » est une simple boîte englobante (pas une énumération
// d'écrans, ex. X11 sans XRandR) : le comptage d'écrans serait faussé.
[[maybe_unused]] State compareTopologies( const QVector<Rect>& physical, const QVector<Rect>& current,
										  bool trustCurrentCount = true )
{
	if( physical.isEmpty() || current.isEmpty() )
	{
		return State::Unknown;
	}
	// Moins d'écrans distants que d'écrans physiques → au moins un moniteur du
	// poste affiche l'hôte physique (hors de la VM) → triche possible.
	if( trustCurrentCount && current.size() < physical.size() )
	{
		return State::NotFullscreen;
	}
	long physicalWidth = 0;
	long physicalHeight = 0;
	long currentWidth = 0;
	long currentHeight = 0;
	boundingBox( physical, physicalWidth, physicalHeight );
	boundingBox( current, currentWidth, currentHeight );
	// La session distante doit couvrir toute l'étendue physique (tolérance 5 %
	// pour l'échelle DPI / arrondis). Sinon : client fenêtré ou partiel → une
	// partie de l'hôte reste visible → triche possible.
	constexpr double coverageRatio = 0.95;
	if( currentWidth < physicalWidth * coverageRatio ||
		currentHeight < physicalHeight * coverageRatio )
	{
		return State::NotFullscreen;
	}
	return State::Fullscreen;
}

}


// ==========================================================================
#if defined(Q_OS_WIN)
// ==========================================================================

// Le client Omnissa Horizon s'exécute sur le POSTE PHYSIQUE de l'étudiant, PAS
// dans cette VM : énumérer les fenêtres de l'invité à la recherche du process
// client (vmware-view/omnissa) est vain — il n'y est jamais → l'ancienne
// implémentation renvoyait toujours Unknown, aucune violation n'était levée.
//
// On raisonne donc côté agent, à partir de deux informations que l'agent Horizon
// publie dans le registre (lisibles par le service Veyon tournant en SYSTEM) :
//   1. Écrans PHYSIQUES du poste étudiant, rapportés par le client :
//      HKLM\SOFTWARE\Omnissa\Horizon\SessionData\<id>\ViewClient_Displays.Topology
//      -> « {largeur,hauteur,gauche,haut,bpp,primaire},{...},... »
//   2. Topologie COURANTE réellement affichée par la session distante :
//      HKLM\SOFTWARE\Omnissa\Horizon\Blast\Telemetry\<id>\ViewClient_Current_Topology
//      -> JSON { "NumDisplays":N, "Displays":[{x,y,width,height,...},...] }
// Si la session distante couvre moins d'écrans, ou une étendue plus petite, que
// le matériel physique, c'est qu'au moins une portion de l'hôte physique reste
// visible/accessible à l'étudiant (client fenêtré, ou plein écran sur un seul des
// écrans) → triche possible → NotFullscreen.
// Aucune donnée Horizon lisible (poste physique, console, hors session) → Unknown
// (pas de faux positif).

namespace
{

// Lit une valeur chaîne (REG_SZ/EXPAND_SZ) sous HKLM. Vue 64 bits forcée : les
// données Horizon vivent dans la ruche native même si Veyon était compilé 32 bits.
QString regReadStringHKLM( const QString& subKey, const QString& valueName )
{
	HKEY key = nullptr;
	if( RegOpenKeyExW( HKEY_LOCAL_MACHINE, reinterpret_cast<const wchar_t*>( subKey.utf16() ),
			0, KEY_READ | KEY_WOW64_64KEY, &key ) != ERROR_SUCCESS )
	{
		return {};
	}
	QString result;
	DWORD type = 0;
	DWORD bytes = 0;
	if( RegQueryValueExW( key, reinterpret_cast<const wchar_t*>( valueName.utf16() ),
			nullptr, &type, nullptr, &bytes ) == ERROR_SUCCESS &&
		( type == REG_SZ || type == REG_EXPAND_SZ ) && bytes > 0 )
	{
		QVector<wchar_t> buffer( static_cast<int>( bytes / sizeof( wchar_t ) ) + 1 );
		DWORD read = bytes;
		if( RegQueryValueExW( key, reinterpret_cast<const wchar_t*>( valueName.utf16() ),
				nullptr, nullptr, reinterpret_cast<LPBYTE>( buffer.data() ), &read ) == ERROR_SUCCESS )
		{
			result = QString::fromWCharArray( buffer.data() );
		}
	}
	RegCloseKey( key );
	return result;
}


// Noms des sous-clés d'une clé HKLM (pour énumérer les sessions).
QStringList regSubKeysHKLM( const QString& subKey )
{
	QStringList names;
	HKEY key = nullptr;
	if( RegOpenKeyExW( HKEY_LOCAL_MACHINE, reinterpret_cast<const wchar_t*>( subKey.utf16() ),
			0, KEY_READ | KEY_WOW64_64KEY, &key ) != ERROR_SUCCESS )
	{
		return names;
	}
	for( DWORD index = 0; ; ++index )
	{
		wchar_t nameBuffer[256];
		DWORD size = 256;
		if( RegEnumKeyExW( key, index, nameBuffer, &size, nullptr, nullptr, nullptr, nullptr ) != ERROR_SUCCESS )
		{
			break;
		}
		names.append( QString::fromWCharArray( nameBuffer, static_cast<int>( size ) ) );
	}
	RegCloseKey( key );
	return names;
}


// Racines registre candidates : Omnissa (actuel) puis VMware historique.
const QStringList& vdiRegistryRoots()
{
	static const QStringList roots{
		QStringLiteral("SOFTWARE\\Omnissa\\Horizon"),
		QStringLiteral("SOFTWARE\\VMware, Inc.\\VMware VDM"),
	};
	return roots;
}


// Parse le JSON ViewClient_Current_Topology : { "Displays":[{x,y,width,height},...] }.
QVector<Rect> parseCurrentTopologyJson( const QString& value )
{
	QVector<Rect> rects;
	const auto document = QJsonDocument::fromJson( value.toUtf8() );
	if( document.isObject() == false )
	{
		return rects;
	}
	const auto displays = document.object().value( QStringLiteral("Displays") ).toArray();
	for( const auto& entry : displays )
	{
		const auto object = entry.toObject();
		Rect r;
		r.left   = object.value( QStringLiteral("x") ).toInt();
		r.top    = object.value( QStringLiteral("y") ).toInt();
		r.width  = object.value( QStringLiteral("width") ).toInt();
		r.height = object.value( QStringLiteral("height") ).toInt();
		if( r.width > 0 && r.height > 0 )
		{
			rects.append( r );
		}
	}
	return rects;
}


// Repli : moniteurs réellement vus par la VM (= ce que la session distante rend).
BOOL CALLBACK collectMonitorProc( HMONITOR monitor, HDC, LPRECT, LPARAM lparam )
{
	MONITORINFO info;
	info.cbSize = sizeof( info );
	if( GetMonitorInfoW( monitor, &info ) )
	{
		const RECT& m = info.rcMonitor;
		reinterpret_cast<QVector<Rect>*>( lparam )->append(
			Rect{ m.left, m.top, m.right - m.left, m.bottom - m.top } );
	}
	return TRUE;
}


QVector<Rect> guestDesktopMonitors()
{
	QVector<Rect> rects;
	EnumDisplayMonitors( nullptr, nullptr, collectMonitorProc, reinterpret_cast<LPARAM>( &rects ) );
	return rects;
}


// Topologie PHYSIQUE du poste étudiant (écrans réels), toutes racines/sessions
// confondues. La première non vide gagne (une session VDI = une seule session).
QVector<Rect> readClientPhysicalTopology()
{
	for( const auto& root : vdiRegistryRoots() )
	{
		const auto sessionRoot = root + QStringLiteral("\\SessionData");
		const auto sessions = regSubKeysHKLM( sessionRoot );
		for( const auto& session : sessions )
		{
			const auto rects = parsePhysicalTopology( regReadStringHKLM(
				sessionRoot + QLatin1Char('\\') + session,
				QStringLiteral("ViewClient_Displays.Topology") ) );
			if( rects.isEmpty() == false )
			{
				return rects;
			}
		}
	}
	return {};
}


// Topologie COURANTE de la session distante. Priorité à la télémétrie Blast
// (même espace de coordonnées que la topologie physique → comparaison homogène,
// insensible à l'échelle DPI) ; repli sur la géométrie réelle du bureau de la VM.
QVector<Rect> readCurrentSessionTopology()
{
	for( const auto& root : vdiRegistryRoots() )
	{
		const auto telemetryRoot = root + QStringLiteral("\\Blast\\Telemetry");
		const auto sessions = regSubKeysHKLM( telemetryRoot );
		for( const auto& session : sessions )
		{
			const auto rects = parseCurrentTopologyJson( regReadStringHKLM(
				telemetryRoot + QLatin1Char('\\') + session,
				QStringLiteral("ViewClient_Current_Topology") ) );
			if( rects.isEmpty() == false )
			{
				return rects;
			}
		}
	}
	return guestDesktopMonitors();
}

}


State fullscreenState()
{
	// Écrans physiques du poste (stables pour la session). Absents → pas de
	// session Horizon détectable → non concluant, aucune violation.
	const auto physical = readClientPhysicalTopology();
	if( physical.isEmpty() )
	{
		return State::Unknown;
	}
	// Ce que la session distante occupe réellement en ce moment.
	return compareTopologies( physical, readCurrentSessionTopology() );
}


bool forceFullscreen()
{
	// Le client VDI tourne sur le poste physique, hors de cette VM : aucun moyen
	// fiable de le forcer en plein écran depuis l'invité. On se contente de
	// constater l'état ; le consommateur enchaîne message + délai de grâce.
	return fullscreenState() == State::Fullscreen;
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

// Deux modes de déploiement couverts :
//
// 1. INVITÉ VDI Linux : le client Horizon tourne sur le poste PHYSIQUE de
//    l'étudiant, pas dans la VM — l'énumération de fenêtres X11 est vaine. On
//    raisonne comme l'agent Windows (registre) : l'agent Horizon pour Linux
//    publie les informations client (ViewClient_*) dans l'ENVIRONNEMENT des
//    processus de la session distante. On y lit ViewClient_Displays.Topology
//    (écrans physiques du poste étudiant, balayage /proc/<pid>/environ — le
//    composant Veyon session y a accès pour ses propres processus, le service
//    root pour tous) et on la compare à la topologie COURANTE réellement
//    affichée par la session (XRandR ; repli : étendue de l'écran X).
//
// 2. POSTE PHYSIQUE : le client tourne localement → détection fenêtre
//    EWMH/X11 ci-dessous (verdict seulement dans ce déploiement).

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


// --- Invité VDI : informations client publiées par l'agent Horizon ---------

// Valeur d'une variable d'environnement ViewClient_* dans les processus de la
// session distante (/proc/<pid>/environ). Le dernier pid porteur est mémorisé
// pour éviter un balayage complet de /proc à chaque tick (2 s).
QString sessionEnvironmentValue( const QByteArray& name )
{
	static long cachedPid = 0;
	const auto readEnviron = [&name]( long pid ) -> QString {
		QFile file( QStringLiteral("/proc/%1/environ").arg( pid ) );
		if( file.open( QIODevice::ReadOnly ) == false )
		{
			return {};
		}
		const auto entries = file.readAll().split( '\0' );
		for( const auto& entry : entries )
		{
			if( entry.size() > name.size() && entry.startsWith( name ) &&
				entry.at( name.size() ) == '=' )
			{
				return QString::fromLocal8Bit( entry.mid( name.size() + 1 ) );
			}
		}
		return {};
	};
	if( cachedPid > 0 )
	{
		const auto value = readEnviron( cachedPid );
		if( value.isEmpty() == false )
		{
			return value;
		}
		cachedPid = 0;		// processus disparu : re-balayer
	}
	const auto entries = QDir( QStringLiteral("/proc") ).entryList( QDir::Dirs | QDir::NoDotAndDotDot );
	for( const auto& entry : entries )
	{
		bool numeric = false;
		const long pid = entry.toLong( &numeric );
		if( numeric == false || pid <= 0 )
		{
			continue;
		}
		const auto value = readEnviron( pid );
		if( value.isEmpty() == false )
		{
			cachedPid = pid;
			return value;
		}
	}
	return {};
}


// Contrairement à la ruche registre Windows, l'agent Horizon POUR LINUX ne
// publie PAS les ViewClient_* dans l'environnement des processus de session : il
// écrit les informations client dans un fichier « Environment.txt » sous son
// répertoire de logs, dès l'établissement de la session (réécrit à chaque
// (re)connexion). C'est là que vit la topologie PHYSIQUE des écrans du poste
// étudiant — même valeur, même format que la ruche Windows.
//   Omnissa (actuel) : /var/log/omnissa/Environment.txt
//   VMware (hérité)   : /var/log/vmware/Environment.txt
// Clés utiles (fichier « Clé: valeur ») :
//   Displays.Topology:   {w,h,x,y,bpp,primaire},{...}
//   Displays.TopologyV2: {w,h,x,y,bpp,primaire,dpi},{...}  (repli ; 4 premiers champs identiques)
const QStringList& vdiEnvironmentFiles()
{
	static const QStringList files{
		QStringLiteral("/var/log/omnissa/Environment.txt"),
		QStringLiteral("/var/log/vmware/Environment.txt"),
	};
	return files;
}


// Dernière valeur (occurrence la plus récente) d'une clé « Clé: valeur » dans le
// premier fichier Environment lisible. Le fichier est un journal : une
// reconnexion ajoute une nouvelle ligne → on garde la DERNIÈRE = session
// courante. Le fichier est world-readable (0644) : lisible tant par le composant
// Veyon en session que par le service root.
QString environmentFileValue( const QByteArray& key )
{
	const QString prefix = QString::fromLatin1( key ) + QLatin1Char(':');
	for( const auto& path : vdiEnvironmentFiles() )
	{
		QFile file( path );
		if( file.open( QIODevice::ReadOnly ) == false )
		{
			continue;
		}
		const auto content = QString::fromUtf8( file.readAll() );
		file.close();
		QString value;
		const auto lines = content.split( QLatin1Char('\n') );
		for( const auto& line : lines )
		{
			// Le « : » du préfixe désambiguïse Displays.Topology de
			// Displays.TopologyV2/V3.
			if( line.startsWith( prefix ) )
			{
				value = line.mid( prefix.size() ).trimmed();
			}
		}
		if( value.isEmpty() == false )
		{
			return value;
		}
	}
	return {};
}


// Écrans PHYSIQUES du poste étudiant, rapportés par le client Horizon.
QVector<Rect> readClientPhysicalTopologyLinux()
{
	// 1) Source réelle sous Linux : fichier Environment de l'agent Horizon.
	static const QByteArray fileKeys[]{
		QByteArrayLiteral("Displays.Topology"),
		QByteArrayLiteral("Displays.TopologyV2"),
	};
	for( const auto& key : fileKeys )
	{
		const auto rects = parsePhysicalTopology( environmentFileValue( key ) );
		if( rects.isEmpty() == false )
		{
			return rects;
		}
	}
	// 2) Repli : variables ViewClient_* dans l'environnement de session (nom
	//    officiel avec point ; variante underscore par prudence). Conservé pour
	//    d'éventuelles versions d'agent qui les exporteraient.
	static const QByteArray envNames[]{
		QByteArrayLiteral("ViewClient_Displays.Topology"),
		QByteArrayLiteral("ViewClient_Displays_Topology"),
	};
	for( const auto& name : envNames )
	{
		const auto rects = parsePhysicalTopology( sessionEnvironmentValue( name ) );
		if( rects.isEmpty() == false )
		{
			return rects;
		}
	}
	return {};
}


// Topologie COURANTE réellement affichée par la session distante : écrans
// XRandR si disponible (enumerated=true, comptage fiable), sinon étendue de
// l'écran X (boîte englobante seule).
QVector<Rect> readCurrentSessionTopologyLinux( Display* display, bool* enumerated )
{
	QVector<Rect> rects;
	*enumerated = false;
	const int screen = DefaultScreen( display );
#if defined(EXAMMODE_HAVE_XRANDR)
	int monitorCount = 0;
	XRRMonitorInfo* monitors = XRRGetMonitors( display, RootWindow( display, screen ), True, &monitorCount );
	if( monitors != nullptr )
	{
		for( int i = 0; i < monitorCount; ++i )
		{
			if( monitors[i].width > 0 && monitors[i].height > 0 )
			{
				rects.append( Rect{ monitors[i].x, monitors[i].y, monitors[i].width, monitors[i].height } );
			}
		}
		XRRFreeMonitors( monitors );
		*enumerated = rects.isEmpty() == false;
	}
#endif
	if( rects.isEmpty() )
	{
		rects.append( Rect{ 0, 0, DisplayWidth( display, screen ), DisplayHeight( display, screen ) } );
	}
	return rects;
}

}


State fullscreenState()
{
	Display* display = openDisplay();
	if( display == nullptr )
	{
		return State::Unknown;
	}

	// 1) Invité VDI : topologie physique du poste étudiant publiée par l'agent
	// Horizon (environnement de session) — même raisonnement que l'agent Windows.
	const auto physical = readClientPhysicalTopologyLinux();
	if( physical.isEmpty() == false )
	{
		bool enumerated = false;
		const auto current = readCurrentSessionTopologyLinux( display, &enumerated );
		XCloseDisplay( display );
		return compareTopologies( physical, current, enumerated );
	}

	// 2) Poste physique : le client tourne localement → détection fenêtre EWMH.
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
	// Invité VDI : le client tourne sur le poste physique, hors d'atteinte de
	// la VM → simple constat de l'état (comme l'agent Windows) ; le consommateur
	// enchaîne message + délai de grâce.
	if( readClientPhysicalTopologyLinux().isEmpty() == false )
	{
		return fullscreenState() == State::Fullscreen;
	}

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
