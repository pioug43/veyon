/*
 * WebApiVncBridge.h - declaration of WebApiVncBridge class
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

#include <QByteArray>
#include <QObject>
#include <QPixmap>
#include <QPoint>
#include <QRegion>
#include <QSize>

#include "ComputerControlInterface.h"

class QWebSocket;

// Bridges a single noVNC client (connected via WebSocket) to an already
// authenticated VncConnection: implements a minimal RFB server (security type
// None, encodages Raw et Tight JPEG) and re-serves the framebuffer of the
// upstream VncConnection while forwarding pointer/keyboard/clipboard input to
// it. Le curseur distant (forme RichCursor + position PointerPos amont, plus
// l'écho des événements pointeur du client) est composité dans les trames.
class WebApiVncBridge : public QObject
{
	Q_OBJECT
public:
	WebApiVncBridge( QWebSocket* socket, ComputerControlInterface::Pointer controlInterface, QObject* parent = nullptr );
	~WebApiVncBridge() override;

private:
	// RFB client-to-server pixel format as negotiated via SetPixelFormat
	struct PixelFormat
	{
		quint8 bitsPerPixel{32};
		quint8 depth{24};
		quint8 bigEndian{0};
		quint8 trueColour{1};
		quint16 redMax{255};
		quint16 greenMax{255};
		quint16 blueMax{255};
		quint8 redShift{16};
		quint8 greenShift{8};
		quint8 blueShift{0};
	};

	// state of the RFB handshake / message loop
	enum class State
	{
		WaitingClientVersion,
		WaitingSecurityType,
		WaitingClientInit,
		Running
	};

	// RFB client-to-server message types
	enum RfbClientMessage
	{
		SetPixelFormatMessage = 0,
		SetEncodingsMessage = 2,
		FramebufferUpdateRequestMessage = 3,
		KeyEventMessage = 4,
		PointerEventMessage = 5,
		ClientCutTextMessage = 6
	};

	// RFB pseudo-encodings we care about
	enum RfbEncoding
	{
		EncodingRaw = 0,
		EncodingTight = 7,
		PseudoEncodingJpegQualityLow = -32,		// -32..-23 = niveau de qualité JPEG 0..9
		PseudoEncodingJpegQualityHigh = -23,
		PseudoEncodingCursor = -239,
		PseudoEncodingDesktopSize = -223,
		PseudoEncodingExtendedDesktopSize = -308
	};

	void handleBinaryMessage( const QByteArray& message );
	bool processBuffer();		// returns false if more data is needed

	void sendServerVersion();
	void sendSecurityTypes();
	void sendSecurityResult();
	void sendServerInit();

	bool parseClientMessage();	// returns false if more data is needed

	void onImageUpdated( int x, int y, int w, int h );
	void onFramebufferUpdateComplete();
	void onFramebufferSizeChanged( int w, int h );
	void onCursorShapeUpdated( const QImage& cursorShape, int xh, int yh );

	void trySendFramebufferUpdate();
	void sendCursorUpdate();		// pousse la forme du curseur (Cursor pseudo-encoding)
	QByteArray encodeCursorRect( const QImage& cursor ) const;
	QByteArray encodeRawRect( const QImage& image, const QRect& rect ) const;
	int appendRect( QByteArray& rectsData, const QImage& image, const QRect& rect );
	QByteArray encodeTightJpegRect( const QImage& image, const QRect& rect ) const;
	bool clientFormatIsNative() const;
	void appendUint16( QByteArray& data, quint16 value ) const;
	void appendUint32( QByteArray& data, quint32 value ) const;

	void closeWithError( const char* reason );
	void setLiveUpdateMode();

	QWebSocket* m_socket{nullptr};
	ComputerControlInterface::Pointer m_controlInterface;

	State m_state{State::WaitingClientVersion};
	QByteArray m_buffer{};
	PixelFormat m_pixelFormat{};

	bool m_supportsDesktopSize{false};
	bool m_supportsExtendedDesktopSize{false};
	bool m_supportsTight{false};		// client accepte l'encodage Tight (JPEG)
	int m_jpegQuality{80};				// qualité JPEG issue du pseudo-encoding qualité du client
	bool m_supportsCursor{false};		// client noVNC accepte le Cursor pseudo-encoding
	bool m_haveCursor{false};			// une forme de curseur a déjà été reçue de l'hôte
	QImage m_cursorShape{};
	QPoint m_cursorHotspot{};

	bool m_updatePending{false};
	bool m_forceFullUpdate{true};
	bool m_pendingResize{false};
	QRegion m_dirtyRegion{};
	QRect m_requestedRect{};
	QSize m_framebufferSize{};

};
