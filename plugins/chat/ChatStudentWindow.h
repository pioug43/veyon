/*
 * ChatStudentWindow.h - fenêtre de chat affichée dans la session utilisateur
 *
 * Contrairement au dialogue du plugin survey, cette fenêtre reste ouverte en
 * arrière-plan pendant toute la durée du chat : la fermer la masque seulement,
 * elle réapparaît à l'arrivée d'un message. Elle n'est réellement détruite
 * qu'à la réception de StopChat.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

#include <QPointer>
#include <QWidget>

#include "Feature.h"

class VeyonWorkerInterface;

namespace Ui { class ChatStudentWindow; }

class ChatStudentWindow : public QWidget
{
	Q_OBJECT
public:
	static void open( Feature::Uid featureUid, VeyonWorkerInterface* worker,
					  bool canSendMessages, bool staysOnTop );
	static void appendTeacherMessage( const QString& text );
	static void shutdown();

protected:
	void closeEvent( QCloseEvent* event ) override;

private:
	ChatStudentWindow( Feature::Uid featureUid, VeyonWorkerInterface* worker,
					   bool canSendMessages, bool staysOnTop );
	~ChatStudentWindow() override;

	void sendMessage();
	void appendMessage( const QString& sender, const QString& text, bool own );
	void popup();

	static QPointer<ChatStudentWindow> s_instance;

	Ui::ChatStudentWindow* ui;
	const Feature::Uid m_featureUid;
	VeyonWorkerInterface* m_worker;
	bool m_terminating{false};
};
