/*
 * CryptoCore.cpp - core functions for crypto features
 *
 * Copyright (c) 2017-2026 Tobias Junghans <tobydox@veyon.io>
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

#include <openssl/bn.h>

#include "CryptoCore.h"

CryptoCore::CryptoCore() :
	m_qcaInitializer(),
	m_defaultPrivateKey()
{
	const auto features = QCA::supportedFeatures();

	vDebug() << "CryptoCore instance created - features supported by QCA" << qcaVersionStr() << features;

	if( features.contains( QStringLiteral( "rsa" ) ) == false )
	{
		qFatal( "CryptoCore: RSA not supported! Please install a QCA plugin which provides RSA support "
				"(e.g. packages such as libqca-qt5-2-plugins or qca-qt5-ossl)." );
	}

	m_defaultPrivateKey = PrivateKey::fromPEMFile( QStringLiteral(":/core/default-pkey.pem") );
}



CryptoCore::~CryptoCore()
{
	vDebug();
}



QByteArray CryptoCore::generateChallenge()
{
	const auto challengeBigNum = BN_new();

	if( challengeBigNum == nullptr )
	{
		vCritical() << "BN_new() failed";
		return QByteArray();
	}

	// generate a random challenge
	// bn2binpad vers un tampon de taille FIXE : sinon un MSB nul (~1/256) donnait
	// un BN_num_bytes() < ChallengeSize, un challenge trop court, et un rejet
	// « challenge.size() != ChallengeSize » côté pair (échec d'auth intermittent).
	QByteArray chall( ChallengeSize, 0 );
	if( BN_rand( challengeBigNum, ChallengeSize * 8, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY ) != 1 ||
		BN_bn2binpad( challengeBigNum, reinterpret_cast<unsigned char *>( chall.data() ), ChallengeSize ) < 0 )
	{
		vCritical() << "BN_rand() failed";
		BN_free( challengeBigNum );
		return QByteArray();
	}
	BN_free( challengeBigNum );

	return chall;
}



QString CryptoCore::encryptPassword( const PlaintextPassword& password ) const
{
	return QString::fromLatin1( m_defaultPrivateKey.toPublicKey().
								encrypt( password, DefaultEncryptionAlgorithm ).toByteArray().toHex() );
}



CryptoCore::PlaintextPassword CryptoCore::decryptPassword( const QString& encryptedPassword ) const
{
	PlaintextPassword decryptedPassword;

	if( PrivateKey( m_defaultPrivateKey ).decrypt( QByteArray::fromHex( encryptedPassword.toUtf8() ),
												   &decryptedPassword, DefaultEncryptionAlgorithm ) )
	{
		return decryptedPassword;
	}

	vCritical() << "failed to decrypt password!";

	return {};
}
