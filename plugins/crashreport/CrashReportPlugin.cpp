/*
 * CrashReportPlugin.cpp - implementation of CrashReportPlugin class
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

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QLockFile>
#include <QThread>
#include <QUrl>

#include "CrashReportPlugin.h"
#include "CrashHandler.h"
#include "Filesystem.h"
#include "VeyonConfiguration.h"


CrashReportPlugin::CrashReportPlugin( QObject* parent ) :
	QObject( parent )
{
	if( VeyonCore::config().crashReportingEnabled() == false )
	{
		return;
	}

	const auto spoolDir = VeyonCore::filesystem().expandPath( VeyonCore::config().crashReportDirectory() );
	if( VeyonCore::filesystem().ensurePathExists( spoolDir ) == false )
	{
		vWarning() << "CrashReport: unable to create spool directory" << spoolDir << "- crash reporting disabled";
		return;
	}

	// Purge des anciens rapports au démarrage (jamais dans le gestionnaire de
	// crash, qui doit rester minimal).
	pruneOldReports( spoolDir, MaxRetainedReports );

	// Téléversement des rapports en attente vers le portail (consolidation des
	// plantages des postes, que le Web API du broker ne peut pas récupérer).
	const auto uploadUrl = VeyonCore::config().crashReportUploadUrl();
	if( uploadUrl.isEmpty() == false )
	{
		const auto uploadToken = VeyonCore::config().crashReportUploadToken();

		// en arrière-plan : le démarrage du composant (relancé justement après un crash)
		// ne doit jamais attendre le portail
		startUpload( spoolDir, uploadUrl, uploadToken );

		// composants de longue durée : nouvel essai périodique, pour ne pas attendre le
		// prochain démarrage quand le portail était injoignable (les VDI non persistantes
		// sont souvent détruites avant)
		const auto component = VeyonCore::component();
		if( component == VeyonCore::Component::Service ||
			component == VeyonCore::Component::Server ||
			component == VeyonCore::Component::Master )
		{
			auto retryTimer = new QTimer( this );
			connect( retryTimer, &QTimer::timeout, this, [spoolDir, uploadUrl, uploadToken]() {
				startUpload( spoolDir, uploadUrl, uploadToken );
			} );
			retryTimer->start( UploadRetryInterval );
		}
	}

	const auto logDir = VeyonCore::filesystem().expandPath( VeyonCore::config().logFileDirectory() );

	CrashHandler::install( spoolDir, componentName(), logDir );

	vDebug() << "CrashReport: crash capture installed for" << componentName() << "-> spool" << spoolDir;
}



QString CrashReportPlugin::componentName()
{
	switch( VeyonCore::component() )
	{
	case VeyonCore::Component::Service: return QStringLiteral("Service");
	case VeyonCore::Component::Server: return QStringLiteral("Server");
	case VeyonCore::Component::Worker: return QStringLiteral("Worker");
	case VeyonCore::Component::Master: return QStringLiteral("Master");
	case VeyonCore::Component::CLI: return QStringLiteral("CLI");
	case VeyonCore::Component::Configurator: return QStringLiteral("Configurator");
	// Composant introduit en amont (4.11.1) par le durcissement de
	// l'assistant d'authentification : ce switch est exhaustif (-Werror=switch),
	// toute nouvelle valeur doit donc être nommée ici.
	case VeyonCore::Component::AuthHelper: return QStringLiteral("AuthHelper");
	}

	return QStringLiteral("Unknown");
}



void CrashReportPlugin::pruneOldReports( const QString& spoolDir, int keep )
{
	// Tous les fichiers d'un même rapport partagent la base « crash-* » ; on
	// trie par date (plus récents d'abord) et on supprime au-delà de `keep`
	// rapports .json OU du budget disque (les minidumps « WithDataSegs »
	// peuvent peser des dizaines de Mo), en emportant les frères (.dmp/.trace).
	QDir dir( spoolDir );
	const auto reports = dir.entryInfoList( { QStringLiteral("crash-*.json") }, QDir::Files, QDir::Time );

	qint64 usedBytes = 0;
	for( int i = 0; i < reports.size(); ++i )
	{
		const auto base = reports.at( i ).completeBaseName();
		const auto siblings = dir.entryInfoList( { base + QStringLiteral(".*") }, QDir::Files );

		qint64 reportBytes = 0;
		for( const auto& sibling : siblings )
		{
			reportBytes += sibling.size();
		}

		if( i >= keep || usedBytes + reportBytes > MaxSpoolBytes )
		{
			for( const auto& sibling : siblings )
			{
				QFile::remove( sibling.absoluteFilePath() );
			}
			continue;
		}
		usedBytes += reportBytes;
	}
}



void CrashReportPlugin::startUpload( const QString& spoolDir, const QString& url, const QString& token )
{
	auto thread = QThread::create( [spoolDir, url, token]() {
		// un seul téléverseur à la fois par poste : au démarrage, service, serveur et
		// workers chargent tous ce plugin en même temps
		QLockFile lock( QDir( spoolDir ).filePath( QStringLiteral(".upload.lock") ) );
		lock.setStaleLockTime( UploadLockStaleTime );
		if( lock.tryLock( 0 ) == false )
		{
			return;
		}
		uploadPendingReports( spoolDir, url, token );
	} );
	// jamais détruit en cours d'exécution : libéré une fois terminé ; si le processus se
	// termine pendant l'envoi, le rapport reste dans le spool et repartira plus tard
	QObject::connect( thread, &QThread::finished, thread, &QObject::deleteLater );
	thread->start( QThread::LowestPriority );
}



void CrashReportPlugin::uploadPendingReports( const QString& spoolDir, const QString& url, const QString& token )
{
	const QUrl endpoint( url );
	if( endpoint.isValid() == false || endpoint.scheme().isEmpty() )
	{
		vWarning() << "CrashReport: invalid upload URL" << url;
		return;
	}

	QDir dir( spoolDir );
	const auto reports = dir.entryInfoList( { QStringLiteral("crash-*.json") }, QDir::Files, QDir::Time );
	if( reports.isEmpty() )
	{
		return;
	}

	QNetworkAccessManager nam;

	for( const auto& fileInfo : reports )
	{
		QFile f( fileInfo.absoluteFilePath() );
		if( f.open( QIODevice::ReadOnly ) == false )
		{
			continue;
		}
		const auto doc = QJsonDocument::fromJson( f.readAll() );
		f.close();
		if( doc.isObject() == false )
		{
			continue;
		}

		// Le portail identifie un rapport par (host, id) ; l'id = base du fichier.
		auto obj = doc.object();
		obj.insert( QStringLiteral("id"), fileInfo.completeBaseName() );

		QNetworkRequest req( endpoint );
		req.setHeader( QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json") );
		req.setRawHeader( QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json") );
		if( token.isEmpty() == false )
		{
			req.setRawHeader( QByteArrayLiteral("X-Crash-Token"), token.toUtf8() );
		}

		// POST borné (5 s) : ne jamais retarder le démarrage du composant.
		QEventLoop loop;
		QTimer timer;
		timer.setSingleShot( true );
		auto* reply = nam.post( req, QJsonDocument( obj ).toJson( QJsonDocument::Compact ) );
		QObject::connect( reply, &QNetworkReply::finished, &loop, &QEventLoop::quit );
		QObject::connect( &timer, &QTimer::timeout, &loop, &QEventLoop::quit );
		timer.start( 5000 );
		loop.exec();

		const int httpStatus = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
		const bool ok = reply->isFinished() && reply->error() == QNetworkReply::NoError && httpStatus / 100 == 2;
		if( reply->isFinished() == false )
		{
			reply->abort();
		}
		delete reply;

		// rapport refusé tel quel (format invalide, trop volumineux) : il ne sera jamais
		// accepté, on le met de côté pour ne pas bloquer les suivants indéfiniment
		if( httpStatus == 413 || httpStatus == 422 )
		{
			vWarning() << "CrashReport: report" << fileInfo.fileName() << "rejected by portal - status" << httpStatus;
			QFile::rename( fileInfo.absoluteFilePath(), fileInfo.absoluteFilePath() + QStringLiteral(".rejected") );
			continue;
		}

		if( ok )
		{
			// Accepté par le portail : on retire le rapport (et ses frères
			// .trace/.dmp) pour ne pas le re-téléverser.
			for( const auto& sibling : dir.entryInfoList( { fileInfo.completeBaseName() + QStringLiteral(".*") }, QDir::Files ) )
			{
				QFile::remove( sibling.absoluteFilePath() );
			}
			vDebug() << "CrashReport: uploaded" << fileInfo.fileName();
		}
		else
		{
			// Portail injoignable / jeton refusé : on conserve le fichier pour
			// le prochain démarrage et on n'insiste pas (démarrage prioritaire).
			vWarning() << "CrashReport: upload failed for" << fileInfo.fileName() << "status" << httpStatus;
			break;
		}
	}
}
