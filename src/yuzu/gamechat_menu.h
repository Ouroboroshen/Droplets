// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QTimer;

namespace Service::Nx2Chat {
struct View;
}

// Droplet's C Menu: GameChat (Service::Nx2Chat) on the online-play server. It opens with the C
// button, from Multiplayer > GameChat, and when a game asks to start a GameChat. Players call
// friends into a GameChat, answer calls, mute or unmute their microphone, set how loud they hear
// each member and choose whether the people in it see their camera. The camera and the
// microphone themselves are chosen elsewhere (the Camera menu, Configure > Audio); this only
// shares them.
class GameChatMenu : public QDialog {
    Q_OBJECT

public:
    explicit GameChatMenu(QWidget* parent = nullptr);
    ~GameChatMenu() override;

    // Shows Nx2Chat's view as it is now (the main window calls it when the view changes).
    void UpdateView();

signals:
    // A GameChat setting changed: save and apply the settings.
    void SettingsChanged();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void RetranslateUI();
    void UpdateCameraText(bool sharing);
    void UpdateVoice(const Service::Nx2Chat::View& view, bool in_room);
    void OnShareToggled(bool checked);
    void CallTicked();

    QLabel* status;
    QLabel* members_label;
    QListWidget* members;
    QLabel* calls;
    QLabel* volume_label;
    QSlider* volume;
    QLabel* volume_value;
    QPushButton* mute;
    QLabel* voice;
    QPushButton* start;
    QPushButton* leave;
    QLabel* friends_label;
    QListWidget* friends;
    QPushButton* invite;
    QLabel* invites_label;
    QListWidget* invites;
    QPushButton* join;
    QPushButton* decline;
    QCheckBox* share;
    QLabel* camera;
    QCheckBox* join_muted;
    QCheckBox* lower_game;
    QLabel* microphone;
    QLabel* privacy;
    QPushButton* close;
    QTimer* refresh_timer;
    int shown_sections = -1; // which parts UpdateView last showed
    bool muted = true;       // the microphone as UpdateView last showed it
};
