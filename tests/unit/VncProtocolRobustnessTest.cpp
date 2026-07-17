/*
 * VncProtocolRobustnessTest.cpp - protocol fragmentation and size-limit tests
 *
 * Copyright (c) 2026 Tobias Junghans <tobydox@veyon.io>
 *
 * This file is part of Veyon - https://veyon.io
 */

#include <QtTest>

#include <cstring>
#include <limits>

#include "rfb/rfbproto.h"

#include "VariantArrayMessage.h"
#include "VncClientProtocol.h"

class DuplexDevice : public QIODevice
{
public:
	DuplexDevice()
	{
		open(QIODevice::ReadWrite);
	}

	bool isSequential() const override
	{
		return true;
	}

	qint64 bytesAvailable() const override
	{
		return m_input.size() + QIODevice::bytesAvailable();
	}

	void appendInput(const QByteArray& data)
	{
		m_input.append(data);
	}

	const QByteArray& output() const
	{
		return m_output;
	}

protected:
	qint64 readData(char* data, qint64 maxSize) override
	{
		const auto size = qMin(maxSize, qint64(m_input.size()));
		if( size <= 0 )
		{
			return 0;
		}
		std::memcpy(data, m_input.constData(), static_cast<size_t>(size));
		m_input.remove(0, static_cast<int>(size));
		return size;
	}

	qint64 writeData(const char* data, qint64 size) override
	{
		m_output.append(data, static_cast<int>(size));
		return size;
	}

private:
	QByteArray m_input{};
	QByteArray m_output{};
};


class VncProtocolRobustnessTest : public QObject
{
	Q_OBJECT

private Q_SLOTS:
	void handlesPipelinedAndFragmentedHandshake();
	void rejectsOversizedCutText();
	void rejectsOversizedVariantMessageEarly();
	void updatesFramebufferSizeOnNewFBSize();
	void acceptsRealRectOnDesktopEnlargedViaExtDesktopSize();

private:
	static void completeHandshake(DuplexDevice& device, VncClientProtocol& protocol);
	static QByteArray framebufferUpdateHeader(uint16_t nRects);
	static QByteArray rectHeader(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t encoding);
};


void VncProtocolRobustnessTest::completeHandshake(DuplexDevice& device, VncClientProtocol& protocol)
{
	protocol.start();
	device.appendInput(QByteArrayLiteral("RFB 003.008\n"));
	QVERIFY(protocol.read());

	const char securityTypes[] = { 1, rfbSecTypeNone };
	device.appendInput(QByteArray(securityTypes, sizeof(securityTypes)));
	QVERIFY(protocol.read());

	const auto authResult = qToBigEndian<uint32_t>(rfbVncAuthOK);
	device.appendInput(QByteArray(reinterpret_cast<const char*>(&authResult), sizeof(authResult)));
	QVERIFY(protocol.read());

	rfbServerInitMsg init{};
	init.framebufferWidth = qToBigEndian<uint16_t>(640);
	init.framebufferHeight = qToBigEndian<uint16_t>(480);
	init.format.bitsPerPixel = 32;
	init.format.depth = 24;
	init.format.trueColour = 1;
	init.nameLength = qToBigEndian<uint32_t>(4);
	device.appendInput(QByteArray(reinterpret_cast<const char*>(&init), sz_rfbServerInitMsg));
	device.appendInput(QByteArrayLiteral("test"));
	QVERIFY(protocol.read());
	QCOMPARE(protocol.state(), VncClientProtocol::Running);
}


QByteArray VncProtocolRobustnessTest::framebufferUpdateHeader(uint16_t nRects)
{
	rfbFramebufferUpdateMsg update{};
	update.type = rfbFramebufferUpdate;
	update.nRects = qToBigEndian<uint16_t>(nRects);
	return QByteArray(reinterpret_cast<const char*>(&update), sz_rfbFramebufferUpdateMsg);
}


QByteArray VncProtocolRobustnessTest::rectHeader(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t encoding)
{
	rfbFramebufferUpdateRectHeader header{};
	header.r.x = qToBigEndian<uint16_t>(x);
	header.r.y = qToBigEndian<uint16_t>(y);
	header.r.w = qToBigEndian<uint16_t>(w);
	header.r.h = qToBigEndian<uint16_t>(h);
	header.encoding = qToBigEndian<uint32_t>(encoding);
	return QByteArray(reinterpret_cast<const char*>(&header), sz_rfbFramebufferUpdateRectHeader);
}


// Non-régression multi-écran : le pseudo-encoding NewFBSize doit actualiser les
// dimensions du framebuffer, sinon le contrôle de bornes rejette ensuite tout
// rectangle de la zone étendue (vignette/prise de main noire).
void VncProtocolRobustnessTest::updatesFramebufferSizeOnNewFBSize()
{
	DuplexDevice device;
	VncClientProtocol protocol(&device, {});
	completeHandshake(device, protocol);
	QCOMPARE(protocol.framebufferWidth(), 640);
	QCOMPARE(protocol.framebufferHeight(), 480);

	device.appendInput(framebufferUpdateHeader(1) +
					   rectHeader(0, 0, 1920, 1080, rfbEncodingNewFBSize));
	QVERIFY(protocol.receiveMessage());
	QCOMPARE(protocol.framebufferWidth(), 1920);
	QCOMPARE(protocol.framebufferHeight(), 1080);
	QVERIFY(device.isOpen());
}


// Non-régression multi-écran : après un ExtDesktopSize vers un bureau 5760×1080
// (3 moniteurs 1920×1080), un rectangle réel situé sur le 3e moniteur (x=3840)
// doit être accepté. Sans la mise à jour des dimensions, il était rejeté
// (« framebuffer update rectangle outside framebuffer ») et la socket fermée —
// l'écran noir observé sur poste VDI Linux à l'extension de l'affichage.
void VncProtocolRobustnessTest::acceptsRealRectOnDesktopEnlargedViaExtDesktopSize()
{
	DuplexDevice device;
	VncClientProtocol protocol(&device, {});
	completeHandshake(device, protocol);

	// ExtDesktopSize : en-tête w/h = nouvelle taille, charge utile = 1 écran
	rfbExtDesktopSizeMsg extDesktopSize{};
	extDesktopSize.numberOfScreens = 1;
	rfbExtDesktopScreen screen{};
	screen.width = qToBigEndian<uint16_t>(5760);
	screen.height = qToBigEndian<uint16_t>(1080);
	device.appendInput(framebufferUpdateHeader(1) +
					   rectHeader(0, 0, 5760, 1080, rfbEncodingExtDesktopSize) +
					   QByteArray(reinterpret_cast<const char*>(&extDesktopSize), sz_rfbExtDesktopSizeMsg) +
					   QByteArray(reinterpret_cast<const char*>(&screen), sz_rfbExtDesktopScreen));
	QVERIFY(protocol.receiveMessage());
	QCOMPARE(protocol.framebufferWidth(), 5760);
	QCOMPARE(protocol.framebufferHeight(), 1080);

	// Rectangle réel (Raw) sur le 3e moniteur : 16×1 pixels à x=3840
	const int rawPixels = 16 * 1 * 4;	// bitsPerPixel=32 (handshake)
	device.appendInput(framebufferUpdateHeader(1) +
					   rectHeader(3840, 0, 16, 1, rfbEncodingRaw) +
					   QByteArray(rawPixels, '\x2a'));
	QVERIFY(protocol.receiveMessage());
	QVERIFY(device.isOpen());
	QCOMPARE(protocol.lastUpdatedRect(), QRect(3840, 0, 16, 1));
}


void VncProtocolRobustnessTest::handlesPipelinedAndFragmentedHandshake()
{
	DuplexDevice device;
	VncClientProtocol protocol(&device, {});
	protocol.start();

	const char firstFragment[] = { 2, rfbSecTypeVncAuth };
	device.appendInput(QByteArrayLiteral("RFB 003.008\n") + QByteArray(firstFragment, sizeof(firstFragment)));
	QVERIFY(protocol.read());
	QCOMPARE(protocol.state(), VncClientProtocol::SecurityInit);

	// The count and first entry must not be consumed until the full list arrives.
	QVERIFY(protocol.read() == false);
	QCOMPARE(protocol.state(), VncClientProtocol::SecurityInit);

	const char secondFragment = rfbSecTypeNone;
	device.appendInput(QByteArray(&secondFragment, 1));
	QVERIFY(protocol.read());
	QCOMPARE(protocol.state(), VncClientProtocol::SecurityChallenge);
}


void VncProtocolRobustnessTest::rejectsOversizedCutText()
{
	DuplexDevice device;
	VncClientProtocol protocol(&device, {});
	completeHandshake(device, protocol);

	rfbServerCutTextMsg message{};
	message.type = rfbServerCutText;
	message.length = qToBigEndian<uint32_t>(std::numeric_limits<uint32_t>::max());
	device.appendInput(QByteArray(reinterpret_cast<const char*>(&message), sz_rfbServerCutTextMsg));

	QVERIFY(protocol.receiveMessage() == false);
	QVERIFY(device.isOpen() == false);
}


void VncProtocolRobustnessTest::rejectsOversizedVariantMessageEarly()
{
	DuplexDevice device;
	const auto size = qToBigEndian<VariantArrayMessage::MessageSize>(64 * 1024 * 1024);
	device.appendInput(QByteArray(reinterpret_cast<const char*>(&size), sizeof(size)));

	VariantArrayMessage message(&device);
	QVERIFY(message.isReadyForReceive() == false);
	QVERIFY(device.isOpen() == false);
}


QTEST_GUILESS_MAIN(VncProtocolRobustnessTest)

#include "VncProtocolRobustnessTest.moc"
