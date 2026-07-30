/*
 * LinuxInputDeviceFunctions.cpp - implementation of LinuxInputDeviceFunctions class
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

#include "PlatformServiceFunctions.h"
#include "InputBlockHelper.h"
#include "LinuxInputDeviceFunctions.h"
#include "LinuxKeyboardShortcutTrapper.h"

// Les en-têtes X11 doivent rester APRÈS les en-têtes projet/Qt : leurs macros
// (None, Success, Bool, Status…) entrent en conflit avec Qt. Ce fichier est pour
// la même raison exclu du build unifié (cf. CMakeLists.txt).
#include <cstring>

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>


LinuxInputDeviceFunctions::LinuxInputDeviceFunctions() :
	m_isWaylandSession(qEnvironmentVariableIsSet("WAYLAND_DISPLAY"))
{
	if( m_isWaylandSession )
	{
		m_inputBlockHelper = new InputBlockHelper;
	}
}



LinuxInputDeviceFunctions::~LinuxInputDeviceFunctions()
{
	ungrabX11InputDevices();
	delete m_inputBlockHelper;
}


void LinuxInputDeviceFunctions::enableInputDevices()
{
	if( m_isWaylandSession )
	{
		// Always send unblock — the daemon tracks state, unblock is idempotent
		enableInputDevicesWayland();
		m_inputDevicesDisabled = false;
		return;
	}

	if( m_inputDevicesDisabled == false )
		return;

	ungrabX11InputDevices();
	m_inputDevicesDisabled = false;
}



void LinuxInputDeviceFunctions::disableInputDevices()
{
	if( m_inputDevicesDisabled )
		return;

	if( m_isWaylandSession )
	{
		disableInputDevicesWayland();
	}
	else if( grabX11InputDevices() == false )
	{
		// On marque quand même le blocage actif : des grabs partiels peuvent
		// exister et enableInputDevices() doit pouvoir les relâcher.
		vCritical() << "failed to block local input devices - the user may still "
					   "be able to interact with this computer";
	}

	m_inputDevicesDisabled = true;
}



KeyboardShortcutTrapper* LinuxInputDeviceFunctions::createKeyboardShortcutTrapper( QObject* parent )
{
	return new LinuxKeyboardShortcutTrapper( parent );
}



// Un périphérique physique (« slave ») est à saisir ; les périphériques maîtres et le
// périphérique virtuel XTEST sont à ignorer — saisir XTEST bloquerait l'injection
// d'événements par Veyon lui-même lors du contrôle à distance.
static bool isGrabbableSlaveDevice( const XIDeviceInfo& device )
{
	if( device.use != XISlavePointer && device.use != XISlaveKeyboard )
		return false;

	return std::strstr( device.name, "XTEST" ) == nullptr;
}



bool LinuxInputDeviceFunctions::grabX11InputDevices()
{
	if( m_x11GrabDisplay )
		return true;

	auto* display = XOpenDisplay( nullptr );
	if( display == nullptr )
	{
		vCritical() << "cannot open X display for input device grabbing";
		return false;
	}

	// XIGrabDevice exige XInput2 ≥ 2.0 : sans la vérification, XIQueryDevice
	// échouerait de façon opaque sur un serveur X sans l'extension.
	int xiMajor = 2;
	int xiMinor = 0;
	if( XIQueryVersion( display, &xiMajor, &xiMinor ) != Success )
	{
		vCritical() << "XInput2 extension not available - cannot block input devices";
		XCloseDisplay( display );
		return false;
	}

	m_x11GrabDisplay = display;

	int deviceCount = 0;
	auto* devices = XIQueryDevice( display, XIAllDevices, &deviceCount );
	if( devices == nullptr )
	{
		vCritical() << "cannot enumerate XInput2 devices";
		return false;
	}

	int grabbed = 0;
	int failed = 0;

	for( int i = 0; i < deviceCount; ++i )
	{
		const auto& device = devices[i];

		if( isGrabbableSlaveDevice( device ) == false )
			continue;

		// mask vide : les événements sont routés vers nous sans qu'on en sélectionne
		// aucun, donc ils sont simplement jetés — c'est le blocage.
		XIEventMask mask{};
		mask.deviceid = device.deviceid;
		mask.mask_len = 0;
		mask.mask = nullptr;

		// Le retour est vérifié : un grab déjà détenu par un autre client (AlreadyGrabbed)
		// ferait échouer le blocage silencieusement, laissant l'utilisateur libre d'agir
		// alors que l'enseignant croit l'écran verrouillé.
		const auto result = XIGrabDevice( display, device.deviceid, DefaultRootWindow( display ),
										  CurrentTime, None, GrabModeAsync, GrabModeAsync,
										  False, &mask );
		if( result == GrabSuccess )
		{
			++grabbed;
		}
		else
		{
			++failed;
			vCritical() << "failed to grab input device" << device.name
						<< "(id" << device.deviceid << ") - error" << result;
		}
	}

	XIFreeDeviceInfo( devices );
	XFlush( display );

	return failed == 0 && grabbed > 0;
}



void LinuxInputDeviceFunctions::ungrabX11InputDevices()
{
	if( m_x11GrabDisplay == nullptr )
		return;

	auto* display = static_cast<Display *>( m_x11GrabDisplay );

	int deviceCount = 0;
	auto* devices = XIQueryDevice( display, XIAllDevices, &deviceCount );

	for( int i = 0; devices != nullptr && i < deviceCount; ++i )
	{
		if( isGrabbableSlaveDevice( devices[i] ) )
		{
			XIUngrabDevice( display, devices[i].deviceid, CurrentTime );
		}
	}

	if( devices != nullptr )
	{
		XIFreeDeviceInfo( devices );
	}

	// La fermeture de la connexion relâcherait de toute façon tous les grabs.
	XCloseDisplay( display );
	m_x11GrabDisplay = nullptr;
}



// ---------------------------------------------------------------------------
// Wayland input device blocking via privileged daemon (EVIOCGRAB)
// ---------------------------------------------------------------------------

void LinuxInputDeviceFunctions::disableInputDevicesWayland()
{
	if (m_inputBlockHelper)
		m_inputBlockHelper->block();
}



void LinuxInputDeviceFunctions::enableInputDevicesWayland()
{
	if (m_inputBlockHelper)
		m_inputBlockHelper->unblock();
}
