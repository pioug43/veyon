/*
 * WebApiHttpServer.h - declaration of WebApiHttpServer class
 *
 * Copyright (c) 2020-2026 Tobias Junghans <tobydox@veyon.io>
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

#pragma once

#include <atomic>

#include <QObject>

#include "WebApiController.h"

class QHttpServer;
class QHttpServerRequest;
class QTcpServer;
class WebApiConfiguration;
#ifdef WEBAPI_HAVE_WEBSOCKETS
class WebApiWebSocketServer;
#endif

class WebApiHttpServer : public QObject
{
	Q_OBJECT
public:
	explicit WebApiHttpServer( const WebApiConfiguration& configuration, QObject* parent = nullptr );
	~WebApiHttpServer() override;

	bool start();

	// Garde-fou anti-crash Qt ≤ 6.7 (QTBUG : QHttpServer::sendResponse écrit la
	// réponse d'un QFuture dans un QTcpSocket déjà détruit quand le client HTTP
	// abandonne sa requête → segfault QAbstractSocket::state()). On retarde la
	// destruction différée des sockets du thread serveur tant que des réponses
	// sont en vol (+ fenêtre de grâce le temps que la continuation écrive).
	bool eventFilter( QObject* watched, QEvent* event ) override;

private:
	enum class Method {
		Get,
		Post,
		Put,
		Delete
	};

	bool setupTls();

	template<Method M>
	static QVariantMap dataFromRequest( const QHttpServerRequest& request );

	template<Method M, typename ... Args>
	bool addRoute( const QString& path,
				  WebApiController::Response(WebApiController::* controllerMethod)( const WebApiController::Request& request,
																					  Args... args ) );

	QString getDebugInformation();

	const WebApiConfiguration& m_configuration;

	QThreadPool m_threadPool{this};

	std::atomic<int> m_pendingResponses{0};
	std::atomic<qint64> m_lastResponseFinishedMs{0};

	WebApiController* m_controller{nullptr};
	QHttpServer* m_server{nullptr};
	QTcpServer* m_tcpServer = nullptr;
#ifdef WEBAPI_HAVE_WEBSOCKETS
	WebApiWebSocketServer* m_webSocketServer{nullptr};
#endif
	bool m_debug = qEnvironmentVariableIsSet("VEYON_WEBAPI_DEBUG");

};
