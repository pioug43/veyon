/*
 * WebApiVncBridge.cpp - implementation of WebApiVncBridge class
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

#include <QAbstractSocket>
#include <QImage>
#include <QtEndian>
#include <QWebSocket>

#include "VncConnection.h"
#include "WebApiVncBridge.h"


// name announced to the noVNC client in ServerInit
static const QByteArray desktopName = QByteArrayLiteral("Veyon WebAPI");


WebApiVncBridge::WebApiVncBridge( QWebSocket* socket, ComputerControlInterface::Pointer controlInterface, QObject* parent ) :
	QObject( parent ),
	m_socket( socket ),
	m_controlInterface( std::move( controlInterface ) )
{
	m_socket->setParent( this );

	connect( m_socket, &QWebSocket::binaryMessageReceived, this, &WebApiVncBridge::handleBinaryMessage );
	connect( m_socket, &QWebSocket::disconnected, this, &QObject::deleteLater );

	auto* vncConnection = m_controlInterface ? m_controlInterface->vncConnection() : nullptr;
	if( vncConnection == nullptr )
	{
		closeWithError( "no VNC connection available" );
		return;
	}

	// the upstream VncConnection runs in its own thread; deliver its signals to
	// our thread (the WebSocket server thread) so that the QWebSocket is only
	// ever touched from the thread that created it
	connect( vncConnection, &VncConnection::imageUpdated,
			 this, &WebApiVncBridge::onImageUpdated, Qt::QueuedConnection );
	connect( vncConnection, &VncConnection::framebufferUpdateComplete,
			 this, &WebApiVncBridge::onFramebufferUpdateComplete, Qt::QueuedConnection );
	connect( vncConnection, &VncConnection::framebufferSizeChanged,
			 this, &WebApiVncBridge::onFramebufferSizeChanged, Qt::QueuedConnection );
	connect( vncConnection, &VncConnection::cursorShapeUpdated,
			 this, &WebApiVncBridge::onCursorShapeUpdated, Qt::QueuedConnection );

	// demander la forme du curseur à l'hôte (Cursor pseudo-encoding côté amont) :
	// sinon le curseur n'est ni dans le framebuffer ni transmis, et noVNC n'affiche
	// aucune souris. On le relaie ensuite au client noVNC (cf. onCursorShapeUpdated).
	// setUseRemoteCursor modifie l'état d'une connexion vivant dans SON thread ; on
	// le planifie là-bas (comme setLiveUpdateMode) — les signaux sont déjà reçus en
	// QueuedConnection, un appel direct ici serait une data race.
	QMetaObject::invokeMethod( vncConnection, [vncConnection]() {
		vncConnection->setUseRemoteCursor( true );
	}, Qt::QueuedConnection );

	// make the upstream connection deliver a continuous, reactive stream
	setLiveUpdateMode();

	// start the RFB handshake by announcing our protocol version
	sendServerVersion();
}



WebApiVncBridge::~WebApiVncBridge()
{
	if( m_socket )
	{
		m_socket->close();
	}
}



void WebApiVncBridge::setLiveUpdateMode()
{
	const auto controlInterface = m_controlInterface;
	if( controlInterface.isNull() )
	{
		return;
	}

	// setUpdateMode() manipulates timers living in the ComputerControlInterface's
	// own thread, so schedule it there
	QMetaObject::invokeMethod( controlInterface.data(), [controlInterface]() {
		controlInterface->setUpdateMode( ComputerControlInterface::UpdateMode::Live );
	}, Qt::QueuedConnection );
}



void WebApiVncBridge::handleBinaryMessage( const QByteArray& message )
{
	m_buffer.append( message );
	processBuffer();
}



bool WebApiVncBridge::processBuffer()
{
	for( ;; )
	{
		switch( m_state )
		{
		case State::WaitingClientVersion:
			if( m_buffer.size() < 12 )
			{
				return false;
			}
			// we only support RFB 003.008 - ignore the actual client version string
			m_buffer.remove( 0, 12 );
			sendSecurityTypes();
			m_state = State::WaitingSecurityType;
			break;

		case State::WaitingSecurityType:
			if( m_buffer.isEmpty() )
			{
				return false;
			}
			else
			{
				const auto securityType = static_cast<quint8>( m_buffer.at( 0 ) );
				m_buffer.remove( 0, 1 );
				if( securityType != 1 )		// only "None" is offered
				{
					closeWithError( "client selected unsupported security type" );
					return false;
				}
				sendSecurityResult();
				m_state = State::WaitingClientInit;
			}
			break;

		case State::WaitingClientInit:
			if( m_buffer.isEmpty() )
			{
				return false;
			}
			m_buffer.remove( 0, 1 );	// shared-flag - ignored
			sendServerInit();
			m_state = State::Running;
			break;

		case State::Running:
			if( parseClientMessage() == false )
			{
				return false;
			}
			break;
		}
	}
}



void WebApiVncBridge::sendServerVersion()
{
	if( m_socket )
	{
		m_socket->sendBinaryMessage( QByteArrayLiteral("RFB 003.008\n") );
	}
}



void WebApiVncBridge::sendSecurityTypes()
{
	if( m_socket )
	{
		// number-of-security-types = 1, security-type = 1 (None)
		m_socket->sendBinaryMessage( QByteArray::fromRawData( "\x01\x01", 2 ) );
	}
}



void WebApiVncBridge::sendSecurityResult()
{
	if( m_socket )
	{
		QByteArray data;
		appendUint32( data, 0 );	// SecurityResult = 0 (OK)
		m_socket->sendBinaryMessage( data );
	}
}



void WebApiVncBridge::sendServerInit()
{
	auto* vncConnection = m_controlInterface ? m_controlInterface->vncConnection() : nullptr;
	if( vncConnection == nullptr || m_socket == nullptr )
	{
		closeWithError( "no VNC connection available" );
		return;
	}

	const QImage image = vncConnection->image();
	m_framebufferSize = image.size();

	QByteArray data;
	appendUint16( data, static_cast<quint16>( qMax( 1, m_framebufferSize.width() ) ) );	// framebuffer-width
	appendUint16( data, static_cast<quint16>( qMax( 1, m_framebufferSize.height() ) ) );	// framebuffer-height

	// server-pixel-format (matches QImage::Format_RGB32, little-endian in memory)
	data.append( static_cast<char>( 32 ) );		// bits-per-pixel
	data.append( static_cast<char>( 24 ) );		// depth
	data.append( static_cast<char>( 0 ) );		// big-endian-flag
	data.append( static_cast<char>( 1 ) );		// true-colour-flag
	appendUint16( data, 255 );					// red-max
	appendUint16( data, 255 );					// green-max
	appendUint16( data, 255 );					// blue-max
	data.append( static_cast<char>( 16 ) );		// red-shift
	data.append( static_cast<char>( 8 ) );		// green-shift
	data.append( static_cast<char>( 0 ) );		// blue-shift
	data.append( 3, '\0' );						// padding

	appendUint32( data, static_cast<quint32>( desktopName.size() ) );	// name-length
	data.append( desktopName );											// name-string

	m_socket->sendBinaryMessage( data );
}



bool WebApiVncBridge::parseClientMessage()
{
	if( m_buffer.isEmpty() )
	{
		return false;
	}

	const auto* buffer = reinterpret_cast<const uchar*>( m_buffer.constData() );
	const auto messageType = static_cast<quint8>( m_buffer.at( 0 ) );

	switch( messageType )
	{
	case SetPixelFormatMessage:
		if( m_buffer.size() < 20 )
		{
			return false;
		}
		// pixel-format starts at offset 4 (1 type + 3 padding)
		// Rejeter un bitsPerPixel non standard : les boucles d'encodage font
		// value >> (byte*8) ; avec bitsPerPixel > 32 (byte >= 4), le décalage
		// atteint/dépasse 32 sur un quint32 => comportement indéfini.
		if( buffer[4] != 8 && buffer[4] != 16 && buffer[4] != 32 )
		{
			return false;
		}
		m_pixelFormat.bitsPerPixel = buffer[4];
		m_pixelFormat.depth = buffer[5];
		m_pixelFormat.bigEndian = buffer[6];
		m_pixelFormat.trueColour = buffer[7];
		m_pixelFormat.redMax = qFromBigEndian<quint16>( buffer + 8 );
		m_pixelFormat.greenMax = qFromBigEndian<quint16>( buffer + 10 );
		m_pixelFormat.blueMax = qFromBigEndian<quint16>( buffer + 12 );
		m_pixelFormat.redShift = buffer[14];
		m_pixelFormat.greenShift = buffer[15];
		m_pixelFormat.blueShift = buffer[16];
		m_buffer.remove( 0, 20 );
		return true;

	case SetEncodingsMessage:
	{
		if( m_buffer.size() < 4 )
		{
			return false;
		}
		const auto numEncodings = qFromBigEndian<quint16>( buffer + 2 );
		const int messageSize = 4 + 4 * numEncodings;
		if( m_buffer.size() < messageSize )
		{
			return false;
		}
		for( int i = 0; i < numEncodings; ++i )
		{
			const auto encoding = static_cast<qint32>( qFromBigEndian<quint32>( buffer + 4 + 4 * i ) );
			if( encoding == PseudoEncodingDesktopSize )
			{
				m_supportsDesktopSize = true;
			}
			else if( encoding == PseudoEncodingExtendedDesktopSize )
			{
				m_supportsExtendedDesktopSize = true;
			}
			else if( encoding == PseudoEncodingCursor )
			{
				m_supportsCursor = true;
			}
		}
		m_buffer.remove( 0, messageSize );
		// le client vient d'annoncer le support du curseur : si une forme est
		// deja connue, on la pousse tout de suite (sinon au prochain update hote)
		if( m_supportsCursor && m_haveCursor )
		{
			sendCursorUpdate();
		}
		return true;
	}

	case FramebufferUpdateRequestMessage:
	{
		if( m_buffer.size() < 10 )
		{
			return false;
		}
		const bool incremental = buffer[1] != 0;
		const auto x = qFromBigEndian<quint16>( buffer + 2 );
		const auto y = qFromBigEndian<quint16>( buffer + 4 );
		const auto w = qFromBigEndian<quint16>( buffer + 6 );
		const auto h = qFromBigEndian<quint16>( buffer + 8 );
		m_requestedRect = QRect( x, y, w, h );
		if( incremental == false )
		{
			m_forceFullUpdate = true;
		}
		m_updatePending = true;
		m_buffer.remove( 0, 10 );
		trySendFramebufferUpdate();
		return true;
	}

	case KeyEventMessage:
	{
		if( m_buffer.size() < 8 )
		{
			return false;
		}
		const bool down = buffer[1] != 0;
		const auto keySym = qFromBigEndian<quint32>( buffer + 4 );
		m_buffer.remove( 0, 8 );
		if( auto* vncConnection = m_controlInterface ? m_controlInterface->vncConnection() : nullptr )
		{
			vncConnection->keyEvent( static_cast<VncConnection::KeyCode>( keySym ), down );
		}
		return true;
	}

	case PointerEventMessage:
	{
		if( m_buffer.size() < 6 )
		{
			return false;
		}
		const auto buttonMask = static_cast<int>( buffer[1] );
		int x = qFromBigEndian<quint16>( buffer + 2 );
		int y = qFromBigEndian<quint16>( buffer + 4 );
		m_buffer.remove( 0, 6 );
		if( auto* vncConnection = m_controlInterface ? m_controlInterface->vncConnection() : nullptr )
		{
			if( m_framebufferSize.isValid() )
			{
				x = qBound( 0, x, m_framebufferSize.width() - 1 );
				y = qBound( 0, y, m_framebufferSize.height() - 1 );
			}
			vncConnection->mouseEvent( x, y, buttonMask );
		}
		return true;
	}

	case ClientCutTextMessage:
	{
		if( m_buffer.size() < 8 )
		{
			return false;
		}
		const auto textLength = qFromBigEndian<quint32>( buffer + 4 );
		// Plafond : sans borne, un ClientCutText hostile fait attendre (et bufferiser)
		// jusqu'à 4 Gio (DoS mémoire) puis le cast quint32→int devient négatif (lecture
		// hors bornes). Un presse-papier légitime est petit → on ferme au-delà.
		constexpr quint32 MaxClientCutTextLength = 1u << 20;	// 1 MiB
		if( textLength > MaxClientCutTextLength )
		{
			vWarning() << "WebApiVncBridge: ClientCutText length" << textLength << "too large - closing";
			m_buffer.clear();
			if( m_socket )
			{
				m_socket->close();
			}
			return false;
		}
		const qint64 messageSize = static_cast<qint64>( 8 ) + textLength;
		if( m_buffer.size() < messageSize )
		{
			return false;
		}
		const auto text = QString::fromLatin1( m_buffer.constData() + 8, static_cast<int>( textLength ) );
		m_buffer.remove( 0, static_cast<int>( messageSize ) );
		if( auto* vncConnection = m_controlInterface ? m_controlInterface->vncConnection() : nullptr )
		{
			vncConnection->clientCut( text );
		}
		return true;
	}

	default:
		closeWithError( "received unknown RFB client message" );
		m_buffer.clear();
		return false;
	}
}



void WebApiVncBridge::onImageUpdated( int x, int y, int w, int h )
{
	m_dirtyRegion += QRect( x, y, w, h );
	trySendFramebufferUpdate();
}



void WebApiVncBridge::onFramebufferUpdateComplete()
{
	trySendFramebufferUpdate();
}



void WebApiVncBridge::onFramebufferSizeChanged( int w, int h )
{
	const QSize newSize( w, h );
	if( newSize == m_framebufferSize )
	{
		return;
	}

	m_framebufferSize = newSize;

	// a resize can only be communicated if the client negotiated a matching
	// pseudo-encoding; otherwise the RFB session can no longer stay consistent
	if( m_state == State::Running &&
		m_supportsDesktopSize == false && m_supportsExtendedDesktopSize == false )
	{
		closeWithError( "framebuffer resized but client does not support DesktopSize" );
		return;
	}

	m_pendingResize = true;
	m_forceFullUpdate = true;
	m_dirtyRegion = QRegion();
	trySendFramebufferUpdate();
}



void WebApiVncBridge::trySendFramebufferUpdate()
{
	if( m_state != State::Running || m_updatePending == false || m_socket == nullptr ||
		m_socket->state() != QAbstractSocket::ConnectedState )
	{
		return;
	}

	auto* vncConnection = m_controlInterface ? m_controlInterface->vncConnection() : nullptr;
	if( vncConnection == nullptr )
	{
		return;
	}

	// snapshot of the current framebuffer (implicitly shared with the live buffer)
	const QImage image = vncConnection->image();
	if( image.isNull() )
	{
		return;		// keep the request pending until a framebuffer is available
	}

	QByteArray rectsData;
	quint16 numRects = 0;

	// announce a pending resize using a DesktopSize/ExtendedDesktopSize pseudo-rect
	if( m_pendingResize && ( m_supportsDesktopSize || m_supportsExtendedDesktopSize ) )
	{
		appendUint16( rectsData, 0 );	// x-position
		appendUint16( rectsData, 0 );	// y-position
		appendUint16( rectsData, static_cast<quint16>( image.width() ) );
		appendUint16( rectsData, static_cast<quint16>( image.height() ) );

		if( m_supportsDesktopSize )
		{
			appendUint32( rectsData, static_cast<quint32>( PseudoEncodingDesktopSize ) );
		}
		else
		{
			appendUint32( rectsData, static_cast<quint32>( PseudoEncodingExtendedDesktopSize ) );
			// ExtendedDesktopSize body: number-of-screens + padding + one screen
			rectsData.append( static_cast<char>( 1 ) );	// number-of-screens
			rectsData.append( 3, '\0' );					// padding
			appendUint32( rectsData, 0 );					// screen id
			appendUint16( rectsData, 0 );					// x-position
			appendUint16( rectsData, 0 );					// y-position
			appendUint16( rectsData, static_cast<quint16>( image.width() ) );
			appendUint16( rectsData, static_cast<quint16>( image.height() ) );
			appendUint32( rectsData, 0 );					// flags
		}

		numRects++;
		m_pendingResize = false;
		m_forceFullUpdate = true;
	}

	QRegion region;
	if( m_forceFullUpdate )
	{
		// full update (first frame or resize): always send the whole framebuffer
		region = QRegion( image.rect() );
	}
	else
	{
		region = m_dirtyRegion & image.rect();

		// honor the region the client asked for (noVNC normally requests the
		// full framebuffer, so this is usually a no-op)
		if( m_requestedRect.isValid() )
		{
			region &= m_requestedRect;
		}
	}

	if( region.isEmpty() && numRects == 0 )
	{
		return;		// nothing to send yet - keep the request pending
	}

	for( const QRect& rect : region )
	{
		if( rect.isEmpty() )
		{
			continue;
		}
		appendUint16( rectsData, static_cast<quint16>( rect.x() ) );
		appendUint16( rectsData, static_cast<quint16>( rect.y() ) );
		appendUint16( rectsData, static_cast<quint16>( rect.width() ) );
		appendUint16( rectsData, static_cast<quint16>( rect.height() ) );
		appendUint32( rectsData, static_cast<quint32>( EncodingRaw ) );
		rectsData += encodeRawRect( image, rect );
		numRects++;
	}

	QByteArray message;
	message.append( static_cast<char>( 0 ) );	// message-type = FramebufferUpdate
	message.append( static_cast<char>( 0 ) );	// padding
	appendUint16( message, numRects );			// number-of-rectangles
	message += rectsData;

	m_socket->sendBinaryMessage( message );

	m_updatePending = false;
	m_forceFullUpdate = false;
	m_dirtyRegion = QRegion();
}



void WebApiVncBridge::onCursorShapeUpdated( const QPixmap& cursorShape, int xh, int yh )
{
	m_cursorShape = cursorShape;
	m_cursorHotspot = QPoint( xh, yh );
	m_haveCursor = true;
	sendCursorUpdate();
}



void WebApiVncBridge::sendCursorUpdate()
{
	if( m_state != State::Running || m_supportsCursor == false || m_haveCursor == false ||
		m_socket == nullptr || m_socket->state() != QAbstractSocket::ConnectedState )
	{
		return;
	}

	// Cursor pseudo-encoding (-239) : forme + hotspot ; noVNC positionne ensuite le
	// curseur sous le pointeur local (curseur cote client). Un curseur 0x0 le masque.
	const QImage cursor = m_cursorShape.toImage().convertToFormat( QImage::Format_ARGB32 );
	const int w = cursor.width();
	const int h = cursor.height();

	QByteArray rectsData;
	appendUint16( rectsData, static_cast<quint16>( w > 0 ? qBound( 0, m_cursorHotspot.x(), w - 1 ) : 0 ) );	// x = hotspot X
	appendUint16( rectsData, static_cast<quint16>( h > 0 ? qBound( 0, m_cursorHotspot.y(), h - 1 ) : 0 ) );	// y = hotspot Y
	appendUint16( rectsData, static_cast<quint16>( w ) );
	appendUint16( rectsData, static_cast<quint16>( h ) );
	appendUint32( rectsData, static_cast<quint32>( PseudoEncodingCursor ) );
	rectsData += encodeCursorRect( cursor );

	QByteArray message;
	message.append( static_cast<char>( 0 ) );	// message-type = FramebufferUpdate
	message.append( static_cast<char>( 0 ) );	// padding
	appendUint16( message, 1 );					// number-of-rectangles
	message += rectsData;

	m_socket->sendBinaryMessage( message );
}



QByteArray WebApiVncBridge::encodeCursorRect( const QImage& cursor ) const
{
	const int w = cursor.width();
	const int h = cursor.height();
	const int bytesPerPixel = qMax( 1, m_pixelFormat.bitsPerPixel / 8 );
	const bool bigEndian = m_pixelFormat.bigEndian != 0;

	QByteArray data;

	// 1) donnees de pixels (format client), comme un rect Raw
	for( int y = 0; y < h; ++y )
	{
		const auto* line = reinterpret_cast<const QRgb*>( cursor.constScanLine( y ) );
		for( int x = 0; x < w; ++x )
		{
			const QRgb pixel = line[x];
			const quint32 r = static_cast<quint32>( qRed( pixel ) ) * m_pixelFormat.redMax / 255;
			const quint32 g = static_cast<quint32>( qGreen( pixel ) ) * m_pixelFormat.greenMax / 255;
			const quint32 b = static_cast<quint32>( qBlue( pixel ) ) * m_pixelFormat.blueMax / 255;
			const quint32 value = ( r << m_pixelFormat.redShift ) |
								  ( g << m_pixelFormat.greenShift ) |
								  ( b << m_pixelFormat.blueShift );
			for( int byte = 0; byte < bytesPerPixel; ++byte )
			{
				const int shift = bigEndian ? ( bytesPerPixel - 1 - byte ) * 8 : byte * 8;
				data.append( static_cast<char>( ( value >> shift ) & 0xFF ) );
			}
		}
	}

	// 2) bitmask 1-bpp (MSB d'abord), lignes alignees a l'octet : bit a 1 = pixel opaque
	const int rowBytes = ( w + 7 ) / 8;
	for( int y = 0; y < h; ++y )
	{
		const auto* line = reinterpret_cast<const QRgb*>( cursor.constScanLine( y ) );
		for( int bx = 0; bx < rowBytes; ++bx )
		{
			quint8 maskByte = 0;
			for( int bit = 0; bit < 8; ++bit )
			{
				const int x = bx * 8 + bit;
				if( x < w && qAlpha( line[x] ) >= 128 )
				{
					maskByte |= static_cast<quint8>( 1 << ( 7 - bit ) );
				}
			}
			data.append( static_cast<char>( maskByte ) );
		}
	}

	return data;
}



QByteArray WebApiVncBridge::encodeRawRect( const QImage& image, const QRect& rect ) const
{
	const int bytesPerPixel = qMax( 1, m_pixelFormat.bitsPerPixel / 8 );
	const bool bigEndian = m_pixelFormat.bigEndian != 0;

	QByteArray data;
	data.resize( static_cast<qsizetype>( rect.width() ) * rect.height() * bytesPerPixel );
	auto* out = reinterpret_cast<uchar*>( data.data() );

	for( int y = rect.top(); y <= rect.bottom(); ++y )
	{
		const auto* line = reinterpret_cast<const QRgb*>( image.constScanLine( y ) );
		for( int x = rect.left(); x <= rect.right(); ++x )
		{
			const QRgb pixel = line[x];
			const quint32 r = static_cast<quint32>( qRed( pixel ) ) * m_pixelFormat.redMax / 255;
			const quint32 g = static_cast<quint32>( qGreen( pixel ) ) * m_pixelFormat.greenMax / 255;
			const quint32 b = static_cast<quint32>( qBlue( pixel ) ) * m_pixelFormat.blueMax / 255;

			const quint32 value = ( r << m_pixelFormat.redShift ) |
								  ( g << m_pixelFormat.greenShift ) |
								  ( b << m_pixelFormat.blueShift );

			for( int byte = 0; byte < bytesPerPixel; ++byte )
			{
				const int shift = bigEndian ? ( bytesPerPixel - 1 - byte ) * 8 : byte * 8;
				*out++ = static_cast<uchar>( ( value >> shift ) & 0xFF );
			}
		}
	}

	return data;
}



void WebApiVncBridge::appendUint16( QByteArray& data, quint16 value ) const
{
	uchar bytes[2];
	qToBigEndian( value, bytes );
	data.append( reinterpret_cast<const char*>( bytes ), 2 );
}



void WebApiVncBridge::appendUint32( QByteArray& data, quint32 value ) const
{
	uchar bytes[4];
	qToBigEndian( value, bytes );
	data.append( reinterpret_cast<const char*>( bytes ), 4 );
}



void WebApiVncBridge::closeWithError( const char* reason )
{
	vWarning() << "WebApiVncBridge:" << reason;
	if( m_socket )
	{
		m_socket->close( QWebSocketProtocol::CloseCodeProtocolError, QString::fromUtf8( reason ) );
	}
}
