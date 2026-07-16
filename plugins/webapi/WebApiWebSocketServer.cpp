/*
 * WebApiWebSocketServer.cpp - implementation of WebApiWebSocketServer class
 *
 * Copyright (c) 2026 Tobias Junghans <tobydox@veyon.io>
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

#include <QHostAddress>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QWebSocket>
#include <QWebSocketServer>

#include "VeyonCore.h"
#include "WebApiConfiguration.h"
#include "WebApiController.h"
#include "WebApiVncBridge.h"
#include "WebApiWebSocketServer.h"


WebApiWebSocketServer::WebApiWebSocketServer( const WebApiConfiguration& configuration,
											  WebApiController* controller, QObject* parent ) :
	QObject( parent ),
	m_configuration( configuration ),
	m_controller( controller ),
	m_server( new QWebSocketServer( QStringLiteral("Veyon WebAPI VNC"),
									QWebSocketServer::NonSecureMode, this ) )
{
	connect( m_server, &QWebSocketServer::newConnection,
			 this, &WebApiWebSocketServer::onNewConnection );
}



WebApiWebSocketServer::~WebApiWebSocketServer()
{
	if( m_server )
	{
		m_server->close();
	}
}



bool WebApiWebSocketServer::start()
{
	const auto port = static_cast<quint16>( m_configuration.webSocketServerPort() );

	if( m_server->listen( QHostAddress::Any, port ) == false )
	{
		vCritical() << "WebAPI WebSocket server can't listen at port" << port
					<< ":" << m_server->errorString();
		return false;
	}

	vInfo() << "WebAPI WebSocket server listening at port" << port;

	return true;
}



void WebApiWebSocketServer::onNewConnection()
{
	while( m_server->hasPendingConnections() )
	{
		auto* socket = m_server->nextPendingConnection();
		if( socket == nullptr )
		{
			break;
		}

		const QUrlQuery query( socket->requestUrl().query() );
		const QUuid uid( query.queryItemValue( QStringLiteral("uid") ) );

		const auto controlInterface = m_controller ? m_controller->lookupConnectionByUid( uid )
													: ComputerControlInterface::Pointer{};
		if( controlInterface.isNull() )
		{
			vWarning() << "WebAPI WebSocket server rejecting connection with invalid uid";
			socket->close( QWebSocketProtocol::CloseCodePolicyViolated,
						   QStringLiteral("invalid connection uid") );
			socket->deleteLater();
			continue;
		}

		// the bridge takes ownership of the socket and deletes itself when the
		// socket is disconnected
		auto* bridge = new WebApiVncBridge( socket, controlInterface, this );

		// une session de pont active ne passe par aucun endpoint HTTP : rien ne
		// ré-arme l'idle-timer de la connexion WebAPI (ConnectionIdleTimeout,
		// 60 s par défaut), qui couperait la prise de main en pleine session.
		// On le ré-arme donc périodiquement tant que le pont est vivant — le
		// timer, enfant du pont, meurt avec lui.
		auto* keepAliveTimer = new QTimer( bridge );
		connect( keepAliveTimer, &QTimer::timeout, this, [this, uid]() {
			if( m_controller )
			{
				m_controller->lookupConnectionByUid( uid );
			}
		} );
		keepAliveTimer->start( ConnectionKeepAliveInterval );
	}
}
