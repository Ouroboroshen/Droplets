// SPDX-FileCopyrightText: Copyright 2026 Droplet
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>
#include <optional>

#include <QDialog>
#include <QPointF>
#include <QWidget>

#include "core/hle/service/sockets/online_notes.h"

class QLabel;
class QTabWidget;
class QTimer;
class QTreeWidget;

// Link on a top-down map: where he has been since the window opened, and where he faces.
class ZeldaNotesMap : public QWidget {
    Q_OBJECT

public:
    explicit ZeldaNotesMap(QWidget* parent = nullptr);

    void Show(const std::optional<Service::Sockets::OnlineRedirect::NotesPlayer>& player);
    void Clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::deque<QPointF> trail; // x, z
    std::optional<Service::Sockets::OnlineRedirect::NotesPlayer> now;
    QString stage;
};

// ZELDA NOTES for Ocarina of Time (Service::Sockets::OnlineRedirect::FetchNotes): the live map,
// what the player has collected and the enemies around Link, while the window is open.
class ZeldaNotesDialog : public QDialog {
    Q_OBJECT

public:
    enum Tab { Map = 0, Collected = 1, Nearby = 2 };

    explicit ZeldaNotesDialog(QWidget* parent = nullptr);
    ~ZeldaNotesDialog() override;

    void ShowTab(Tab tab);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void Poll();
    void Use(const Service::Sockets::OnlineRedirect::NotesView& view);
    void Stop();

    QLabel* status;
    QTabWidget* tabs;
    ZeldaNotesMap* map;
    QLabel* place;
    QTreeWidget* collected;
    QTreeWidget* nearby;
    QLabel* nearby_note;
    QLabel* privacy;
    QTimer* timer;

    std::optional<Service::Sockets::OnlineRedirect::Profile> profile;
    bool started = false; // the server has this window's session
    bool busy = false;    // a call is out
    int generation = 0;   // answers from before the window last closed are dropped
};
