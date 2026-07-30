/*
 * LinuxInputDeviceFunctions.h - declaration of LinuxInputDeviceFunctions class
 *
 * Copyright (c) 2017-2026 Tobias Junghans <tobydox@veyon.io>
 *
 * This file is part of Veyon - https://veyon.io
 */

#pragma once

#include "PlatformInputDeviceFunctions.h"

class InputBlockHelper;

class LinuxInputDeviceFunctions : public PlatformInputDeviceFunctions
{
public:
	LinuxInputDeviceFunctions();
	~LinuxInputDeviceFunctions();

	void enableInputDevices() override;
	void disableInputDevices() override;

	KeyboardShortcutTrapper* createKeyboardShortcutTrapper( QObject* parent ) override;

private:
	bool grabX11InputDevices();
	void ungrabX11InputDevices();

	void disableInputDevicesWayland();
	void enableInputDevicesWayland();

	bool m_inputDevicesDisabled{false};

	const bool m_isWaylandSession;
	InputBlockHelper* m_inputBlockHelper{nullptr};

	// Display* dédié au grab XInput2 : le grab est relâché par le serveur X dès que
	// cette connexion se ferme, y compris si veyon-server meurt. C'est tout l'intérêt
	// par rapport à l'ancienne keymap vide, qui restait en place après un crash.
	void* m_x11GrabDisplay{nullptr};
};
