/*
 * LinuxInputDeviceFunctions.h - declaration of LinuxInputDeviceFunctions class
 *
 * Copyright (c) 2017-2026 Tobias Junghans <tobydox@veyon.io>
 *
 * This file is part of Veyon - https://veyon.io
 */

#pragma once

#include <QSet>

#include "PlatformInputDeviceFunctions.h"

class InputBlockHelper;
class QSocketNotifier;

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
	int grabPendingX11Devices();
	void processX11HierarchyEvents();

	void disableInputDevicesWayland();
	void enableInputDevicesWayland();

	bool m_inputDevicesDisabled{false};

	const bool m_isWaylandSession;
	InputBlockHelper* m_inputBlockHelper{nullptr};

	// Display* dédié au grab XInput2 : le grab est relâché par le serveur X dès que
	// cette connexion se ferme, y compris si veyon-server meurt. C'est tout l'intérêt
	// par rapport à l'ancienne keymap vide, qui restait en place après un crash.
	void* m_x11GrabDisplay{nullptr};

	// Un grab XInput2 porte sur un périphérique précis : sans surveillance, un clavier
	// ou une souris branché PENDANT un blocage échapperait à celui-ci. On s'abonne donc
	// à XI_HierarchyChanged pour saisir les périphériques qui apparaissent.
	QSocketNotifier* m_x11EventNotifier{nullptr};
	QSet<int> m_grabbedX11Devices;
	int m_xiOpcode{0};
};
