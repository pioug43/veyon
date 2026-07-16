/*
 * WebApiWebSocketServer.h - declaration of WebApiWebSocketServer class
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

#pragma once

#include <QObject>

class QWebSocketServer;
class WebApiConfiguration;
class WebApiController;

// Exposes a WebSocket endpoint (ws://<broker>:<port>/vnc?uid=<connection-uid>)
// that lets an external noVNC client attach to an already authenticated WebAPI
// connection. Each accepted socket gets its own WebApiVncBridge.
class WebApiWebSocketServer : public QObject
{
	Q_OBJECT
public:
	WebApiWebSocketServer( const WebApiConfiguration& configuration, WebApiController* controller,
						   QObject* parent = nullptr );
	~WebApiWebSocketServer() override;

	bool start();

private:
	// période de ré-armement de l'idle-timer de la connexion WebAPI pendant
	// une session de pont active — doit rester < ConnectionIdleTimeout (60 s)
	static constexpr int ConnectionKeepAliveInterval = 20000;

	void onNewConnection();

	const WebApiConfiguration& m_configuration;
	WebApiController* m_controller{nullptr};
	QWebSocketServer* m_server{nullptr};

};
