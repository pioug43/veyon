/*
 * ExamModeLinuxExecGuard.cpp - kernel-mediated launch prevention on Linux
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

#include "ExamModeLinuxExecGuard.h"
#include "VeyonCore.h"

#include <QFile>
#include <QSet>

#if defined(Q_OS_LINUX)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/fanotify.h>
#include <linux/limits.h>
#endif


#if defined(Q_OS_LINUX)
namespace
{

QString decodeMountPath( QString path )
{
	path.replace( QStringLiteral("\\040"), QStringLiteral(" ") );
	path.replace( QStringLiteral("\\011"), QStringLiteral("\t") );
	path.replace( QStringLiteral("\\012"), QStringLiteral("\n") );
	path.replace( QStringLiteral("\\134"), QStringLiteral("\\") );
	return path;
}


QStringList executableMounts()
{
	QFile file( QStringLiteral("/proc/self/mountinfo") );
	if( file.open( QIODevice::ReadOnly | QIODevice::Text ) == false )
	{
		return { QStringLiteral("/") };
	}
	const QSet<QString> pseudoFilesystems{
		QStringLiteral("proc"), QStringLiteral("sysfs"), QStringLiteral("devtmpfs"),
		QStringLiteral("devpts"), QStringLiteral("cgroup"), QStringLiteral("cgroup2"),
		QStringLiteral("securityfs"), QStringLiteral("tracefs"), QStringLiteral("debugfs"),
		QStringLiteral("pstore"), QStringLiteral("mqueue"), QStringLiteral("fusectl"),
		QStringLiteral("configfs"),
	};
	QSet<QString> mounts{ QStringLiteral("/") };
	while( file.atEnd() == false )
	{
		const auto line = QString::fromUtf8( file.readLine() ).trimmed();
		const auto separator = line.indexOf( QStringLiteral(" - ") );
		if( separator < 0 ) { continue; }
		const auto left = line.left( separator ).split( QLatin1Char(' '), Qt::SkipEmptyParts );
		const auto right = line.mid( separator + 3 ).split( QLatin1Char(' '), Qt::SkipEmptyParts );
		if( left.size() < 6 || right.isEmpty() || pseudoFilesystems.contains( right.first() ) ||
			left.at( 5 ).split( QLatin1Char(',') ).contains( QStringLiteral("noexec") ) )
		{
			continue;
		}
		mounts.insert( decodeMountPath( left.at( 4 ) ) );
	}
	auto result = mounts.values();
	result.sort();
	return result;
}

}
#endif


ExamModeLinuxExecGuard::ExamModeLinuxExecGuard( QObject* parent ) :
	QThread( parent )
{
}



ExamModeLinuxExecGuard::~ExamModeLinuxExecGuard()
{
	stopGuarding();
}



QString ExamModeLinuxExecGuard::normalizeBasename( const QString& name )
{
	auto base = name.trimmed().section( QLatin1Char('/'), -1 ).section( QLatin1Char('\\'), -1 );
	if( base.endsWith( QStringLiteral(".exe"), Qt::CaseInsensitive ) )
	{
		base.chop( 4 );
	}
	return base.toLower();
}



QSet<QString> ExamModeLinuxExecGuard::blockedSnapshot()
{
	QMutexLocker locker( &m_mutex );
	return m_blocked;
}



void ExamModeLinuxExecGuard::updateBlocked( const QStringList& blockedBasenames )
{
	QSet<QString> set;
	for( const auto& name : blockedBasenames )
	{
		const auto base = normalizeBasename( name );
		if( base.isEmpty() == false )
		{
			set.insert( base );
		}
	}
	QMutexLocker locker( &m_mutex );
	m_blocked = set;
}


#if defined(Q_OS_LINUX)

bool ExamModeLinuxExecGuard::startGuarding( const QStringList& blockedBasenames )
{
	if( m_active.load() )
	{
		updateBlocked( blockedBasenames );
		return true;
	}
	if( m_fanotifyFd >= 0 || m_stopFd >= 0 )
	{
		stopGuarding(); // nettoie les descripteurs d'une boucle terminée en erreur
	}

	updateBlocked( blockedBasenames );

	// FAN_CLASS_CONTENT : autorise les événements de permission (PERM).
	m_fanotifyFd = fanotify_init( FAN_CLOEXEC | FAN_CLASS_CONTENT | FAN_NONBLOCK,
								  O_RDONLY | O_CLOEXEC );
	if( m_fanotifyFd < 0 )
	{
		vWarning() << "ExamMode: fanotify_init indisponible (privilèges/kernel):" << strerror( errno );
		return false;
	}

	// Une marque sur / ne couvre pas les systèmes de fichiers montés dessous
	// (volumes applicatifs, /home séparé, images VDI). Marquer chaque montage
	// exécutable ferme ce contournement. L'échec sur / est fatal ; les montages
	// apparus ensuite sont vérifiés par le contrôle de dérive de l'appelant.
	bool rootMarked = false;
	int markedMounts = 0;
	for( const auto& mount : executableMounts() )
	{
		const auto nativePath = QFile::encodeName( mount );
		const bool marked = fanotify_mark( m_fanotifyFd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM,
			FAN_OPEN_EXEC_PERM, AT_FDCWD, nativePath.constData() ) == 0 ||
			fanotify_mark( m_fanotifyFd, FAN_MARK_ADD | FAN_MARK_MOUNT,
				FAN_OPEN_EXEC_PERM, AT_FDCWD, nativePath.constData() ) == 0;
		if( marked )
		{
			++markedMounts;
			rootMarked = rootMarked || mount == QStringLiteral("/");
		}
		else
		{
			vWarning() << "ExamMode: montage fanotify non couvert" << mount << strerror( errno );
		}
	}
	if( rootMarked == false )
	{
		vWarning() << "ExamMode: fanotify_mark de / a échoué:" << strerror( errno );
		close( m_fanotifyFd );
		m_fanotifyFd = -1;
		return false;
	}

	m_stopFd = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK );
	if( m_stopFd < 0 )
	{
		close( m_fanotifyFd );
		m_fanotifyFd = -1;
		return false;
	}

	m_stopping = false;
	m_active = true;
	m_healthy = true;
	start();
	vInfo() << "ExamMode: prévention de lancement Linux active sur" << markedMounts << "montage(s)";
	return true;
}



void ExamModeLinuxExecGuard::run()
{
	// tampon aligné sur la structure d'événement fanotify.
	alignas(struct fanotify_event_metadata) char buffer[8192];

	// En cas d'incompatibilité d'ABI on bascule en fail-open : on continue de
	// vider la file et d'autoriser chaque exec (sinon les execve en attente
	// resteraient bloqués), mais on ne journalise l'anomalie qu'une fois.
	bool abiMismatchReported = false;

	while( true )
	{
		struct pollfd fds[2];
		fds[0].fd = m_fanotifyFd;
		fds[0].events = POLLIN;
		fds[0].revents = 0;
		fds[1].fd = m_stopFd;
		fds[1].events = POLLIN;
		fds[1].revents = 0;

		const int ready = poll( fds, 2, -1 );
		if( ready < 0 )
		{
			if( errno == EINTR )
			{
				continue;
			}
			break;
		}

		if( fds[1].revents & POLLIN )
		{
			break;		// demande d'arrêt
		}
		if( ( fds[0].revents & POLLIN ) == 0 )
		{
			continue;
		}

		const ssize_t len = read( m_fanotifyFd, buffer, sizeof( buffer ) );
		if( len <= 0 )
		{
			if( len < 0 && ( errno == EAGAIN || errno == EINTR ) )
			{
				continue;
			}
			break;
		}

		const struct fanotify_event_metadata* meta =
			reinterpret_cast<const struct fanotify_event_metadata*>( buffer );
		auto remaining = len;
		while( FAN_EVENT_OK( meta, remaining ) )
		{
			if( meta->vers != FANOTIFY_METADATA_VERSION )
			{
				// Incompatibilité ABI : fail-open. Il faut TOUJOURS répondre à un
				// événement de permission en attente, sinon l'execve concerné reste
				// bloqué indéfiniment (l'inverse du fail-safe visé). On autorise
				// l'exec courant et on ferme son fd ; on cesse de traiter ce lot.
				if( meta->fd >= 0 )
				{
					struct fanotify_response response;
					response.fd = meta->fd;
					response.response = FAN_ALLOW;
					ssize_t written;
					do
					{
						written = write( m_fanotifyFd, &response, sizeof( response ) );
					}
					while( written < 0 && errno == EINTR );
					close( meta->fd );
				}
				if( abiMismatchReported == false )
				{
					vWarning() << "ExamMode: version d'ABI fanotify inattendue — autorisation systématique (fail-open)";
					abiMismatchReported = true;
				}
				remaining = 0;
				break;
			}
			if( meta->fd >= 0 )
			{
				bool deny = false;
				if( meta->mask & FAN_OPEN_EXEC_PERM )
				{
					char path[PATH_MAX];
					const auto link = QStringLiteral("/proc/self/fd/%1").arg( meta->fd );
					const ssize_t n = readlink( link.toLocal8Bit().constData(), path, sizeof( path ) - 1 );
					if( n > 0 )
					{
						path[n] = '\0';
						const auto base = normalizeBasename( QString::fromLocal8Bit( path, static_cast<int>( n ) ) );
						deny = blockedSnapshot().contains( base );
						if( deny )
						{
							vWarning() << "ExamMode: lancement bloqué (fanotify):"
									   << QString::fromLocal8Bit( path );
						}
					}

					// Toujours répondre, même en cas d'erreur (sinon l'execve reste
					// bloqué). Par défaut : autoriser (liste noire explicite).
					struct fanotify_response response;
					response.fd = meta->fd;
					response.response = deny ? FAN_DENY : FAN_ALLOW;
					ssize_t written;
					do
					{
						written = write( m_fanotifyFd, &response, sizeof( response ) );
					}
					while( written < 0 && errno == EINTR );
				}
				close( meta->fd );
			}
			meta = FAN_EVENT_NEXT( meta, remaining );
		}
	}

	// Un retour inattendu de la boucle signifie que la garantie noyau est perdue.
	// Publier l'état avant de quitter permet au contrôle de dérive de réagir.
	m_healthy = false;
	m_active = false;
	if( m_stopping.load() == false )
	{
		vWarning() << "ExamMode: garde fanotify interrompue de façon inattendue";
	}
}



bool ExamModeLinuxExecGuard::refreshMounts()
{
	if( m_fanotifyFd < 0 || m_active.load() == false )
	{
		return false;
	}
	bool rootMarked = false;
	for( const auto& mount : executableMounts() )
	{
		const auto nativePath = QFile::encodeName( mount );
		const bool marked = fanotify_mark( m_fanotifyFd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM,
			FAN_OPEN_EXEC_PERM, AT_FDCWD, nativePath.constData() ) == 0 ||
			fanotify_mark( m_fanotifyFd, FAN_MARK_ADD | FAN_MARK_MOUNT,
				FAN_OPEN_EXEC_PERM, AT_FDCWD, nativePath.constData() ) == 0;
		rootMarked = rootMarked || ( mount == QStringLiteral("/") && marked );
	}
	if( rootMarked == false )
	{
		m_healthy = false;
	}
	return rootMarked;
}



void ExamModeLinuxExecGuard::stopGuarding()
{
	if( m_active.load() == false && isRunning() == false && m_fanotifyFd < 0 && m_stopFd < 0 )
	{
		return;
	}
	m_stopping = true;
	m_active = false;
	m_healthy = false;

	if( m_stopFd >= 0 )
	{
		const uint64_t one = 1;
		ssize_t ignored = write( m_stopFd, &one, sizeof( one ) );
		Q_UNUSED(ignored)
	}

	wait();		// attend la fin de run()

	if( m_fanotifyFd >= 0 )
	{
		close( m_fanotifyFd );		// fermeture => execve en attente autorisés
		m_fanotifyFd = -1;
	}
	if( m_stopFd >= 0 )
	{
		close( m_stopFd );
		m_stopFd = -1;
	}
	m_stopping = false;
}

#else  // plateformes non Linux : non-op

bool ExamModeLinuxExecGuard::startGuarding( const QStringList& blockedBasenames )
{
	Q_UNUSED(blockedBasenames)
	return false;
}

bool ExamModeLinuxExecGuard::refreshMounts()
{
	return false;
}

void ExamModeLinuxExecGuard::run()
{
}

void ExamModeLinuxExecGuard::stopGuarding()
{
	m_active = false;
	m_healthy = false;
}

#endif
