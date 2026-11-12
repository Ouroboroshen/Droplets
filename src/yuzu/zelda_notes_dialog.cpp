// SPDX-FileCopyrightText: Copyright 2026 Droplet
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <utility>

#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include "core/hle/service/sockets/online_redirect.h"
#include "yuzu/zelda_notes_dialog.h"

namespace OnlineRedirect = Service::Sockets::OnlineRedirect;

namespace {

constexpr size_t TrailMax = 900; // about 7 minutes at the game's four updates a second

// The game's names for its checklist flags, as the window shows them
QString CollectedName(const std::string& name) {
    static const std::pair<const char*, const char*> names[] = {
        {"heart_pieces", QT_TRANSLATE_NOOP("ZeldaNotesDialog", "Pieces of Heart")},
        {"gold_skulltula", QT_TRANSLATE_NOOP("ZeldaNotesDialog", "Gold Skulltulas")},
        {"treasure_box", QT_TRANSLATE_NOOP("ZeldaNotesDialog", "Treasure chests")},
        {"hidden_grotto", QT_TRANSLATE_NOOP("ZeldaNotesDialog", "Hidden grottos")},
        {"soft_soil", QT_TRANSLATE_NOOP("ZeldaNotesDialog", "Soft soil (magic beans)")},
        {"business_scrub", QT_TRANSLATE_NOOP("ZeldaNotesDialog", "Business Scrubs")},
        {"defeated_enemy", QT_TRANSLATE_NOOP("ZeldaNotesDialog", "Enemies defeated")},
        {"location", QT_TRANSLATE_NOOP("ZeldaNotesDialog", "Places visited")},
        {"room", QT_TRANSLATE_NOOP("ZeldaNotesDialog", "Rooms visited")},
        {"floor", QT_TRANSLATE_NOOP("ZeldaNotesDialog", "Floors visited")},
        {"episode", QT_TRANSLATE_NOOP("ZeldaNotesDialog", "Story events")},
        {"icon", QT_TRANSLATE_NOOP("ZeldaNotesDialog", "Map icons")},
        {"state", QT_TRANSLATE_NOOP("ZeldaNotesDialog", "Map states")},
        {"knowledge", QT_TRANSLATE_NOOP("ZeldaNotesDialog", "Things learned")},
        {"ocarina_score", QT_TRANSLATE_NOOP("ZeldaNotesDialog", "Ocarina songs")},
        {"scarecrow", QT_TRANSLATE_NOOP("ZeldaNotesDialog", "Scarecrow")},
    };
    for (const auto& [key, text] : names) {
        if (name == key) {
            return QCoreApplication::translate("ZeldaNotesDialog", text);
        }
    }
    return QString::fromStdString(name);
}

} // namespace

ZeldaNotesMap::ZeldaNotesMap(QWidget* parent) : QWidget(parent) {
    setMinimumSize(360, 300);
    setAutoFillBackground(true);
}

void ZeldaNotesMap::Clear() {
    trail.clear();
    now.reset();
    stage.clear();
    update();
}

void ZeldaNotesMap::Show(const std::optional<OnlineRedirect::NotesPlayer>& player) {
    if (player) {
        const QString new_stage = QString::fromStdString(player->stage);
        if (new_stage != stage) {
            trail.clear(); // another place: its coordinates don't mix with the last one's
            stage = new_stage;
        }
        const QPointF point(player->pos[0], player->pos[2]);
        if (trail.empty() || std::hypot(trail.back().x() - point.x(), trail.back().y() - point.y()) > 1.0) {
            trail.push_back(point);
            if (trail.size() > TrailMax) {
                trail.pop_front();
            }
        }
    }
    now = player;
    update();
}

void ZeldaNotesMap::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF area = rect().adjusted(16, 16, -16, -16);
    painter.fillRect(rect(), palette().base());
    if (trail.empty() || !now) {
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(rect(), Qt::AlignCenter, tr("Link appears here when the game sends where he is."));
        return;
    }
    // Fit the trail with a margin of at least 300 units, so standing still isn't a zoomed blur
    double min_x = trail.front().x(), max_x = min_x, min_z = trail.front().y(), max_z = min_z;
    for (const QPointF& p : trail) {
        min_x = std::min(min_x, p.x());
        max_x = std::max(max_x, p.x());
        min_z = std::min(min_z, p.y());
        max_z = std::max(max_z, p.y());
    }
    const double span = std::max({max_x - min_x, max_z - min_z, 300.0}) * 1.15;
    const double cx = (min_x + max_x) / 2, cz = (min_z + max_z) / 2;
    const double scale = std::min(area.width(), area.height()) / span;
    const auto to_screen = [&](double x, double z) {
        return QPointF(area.center().x() + (x - cx) * scale, area.center().y() + (z - cz) * scale);
    };
    // Grid every 500 units
    painter.setPen(QPen(palette().color(QPalette::Mid), 1, Qt::DotLine));
    for (double g = std::floor((cx - span / 2) / 500) * 500; g <= cx + span / 2; g += 500) {
        painter.drawLine(to_screen(g, cz - span / 2), to_screen(g, cz + span / 2));
    }
    for (double g = std::floor((cz - span / 2) / 500) * 500; g <= cz + span / 2; g += 500) {
        painter.drawLine(to_screen(cx - span / 2, g), to_screen(cx + span / 2, g));
    }
    // The trail
    QPainterPath path(to_screen(trail.front().x(), trail.front().y()));
    for (const QPointF& p : trail) {
        path.lineTo(to_screen(p.x(), p.y()));
    }
    painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
    painter.drawPath(path);
    // Link, and where he faces
    const QPointF at = to_screen(now->pos[0], now->pos[2]);
    const double fx = now->front[0], fz = now->front[2];
    const double length = std::hypot(fx, fz);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(46, 160, 67));
    painter.drawEllipse(at, 7, 7);
    if (length > 0.01) {
        painter.setPen(QPen(QColor(46, 160, 67), 3, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(at, at + QPointF(fx / length, fz / length) * 22);
    }
}

ZeldaNotesDialog::ZeldaNotesDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("ZELDA NOTES"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    auto* layout = new QVBoxLayout(this);
    status = new QLabel(this);
    status->setTextFormat(Qt::PlainText); // shows the server's error text
    status->setWordWrap(true);
    layout->addWidget(status);

    tabs = new QTabWidget(this);
    auto* map_page = new QWidget(tabs);
    auto* map_layout = new QVBoxLayout(map_page);
    map = new ZeldaNotesMap(map_page);
    place = new QLabel(map_page);
    place->setTextFormat(Qt::PlainText); // stage and zone names from the server
    place->setWordWrap(true);
    map_layout->addWidget(map, 1);
    map_layout->addWidget(place);
    tabs->addTab(map_page, tr("Live Map"));

    auto* collected_page = new QWidget(tabs);
    auto* collected_layout = new QVBoxLayout(collected_page);
    collected = new QTreeWidget(collected_page);
    collected->setRootIsDecorated(false);
    collected->setHeaderLabels({tr("Checklist"), tr("Marked")});
    collected->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    auto* collected_note = new QLabel(
        tr("The game's own checklist flags, as it keeps them for ZELDA NOTES. It sends them when this "
           "window opens."),
        collected_page);
    collected_note->setWordWrap(true);
    collected_layout->addWidget(collected, 1);
    collected_layout->addWidget(collected_note);
    tabs->addTab(collected_page, tr("Collected"));

    auto* nearby_page = new QWidget(tabs);
    auto* nearby_layout = new QVBoxLayout(nearby_page);
    nearby = new QTreeWidget(nearby_page);
    nearby->setRootIsDecorated(false);
    nearby->setHeaderLabels({tr("Enemy"), tr("Distance"), tr("Life"), tr("Fighting")});
    nearby_note = new QLabel(nearby_page);
    nearby_note->setWordWrap(true);
    nearby_layout->addWidget(nearby, 1);
    nearby_layout->addWidget(nearby_note);
    tabs->addTab(nearby_page, tr("Nearby Enemies"));
    layout->addWidget(tabs, 1);

    privacy = new QLabel(tr("While this window is open, Ocarina of Time sends Link's position and "
                            "checklist to your online server, for this window only. Closing it stops "
                            "that."),
                         this);
    privacy->setWordWrap(true);
    layout->addWidget(privacy);

    timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, &ZeldaNotesDialog::Poll);
    resize(560, 560);
}

ZeldaNotesDialog::~ZeldaNotesDialog() {
    Stop();
}

void ZeldaNotesDialog::ShowTab(Tab tab) {
    tabs->setCurrentIndex(static_cast<int>(tab));
}

void ZeldaNotesDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    const std::string server = OnlineRedirect::ServerAddress();
    profile = server.empty() ? std::nullopt : OnlineRedirect::LoadProfile(server, false);
    if (!profile) {
        status->setText(tr("ZELDA NOTES needs online play: turn it on in Configure > System > Network, "
                           "then play Ocarina of Time online."));
        return;
    }
    status->setText(tr("Asking the game…"));
    map->Clear();
    Poll();
    timer->start();
}

void ZeldaNotesDialog::hideEvent(QHideEvent* event) {
    timer->stop();
    Stop();
    QDialog::hideEvent(event);
}

void ZeldaNotesDialog::Stop() {
    ++generation;
    busy = false;
    if (!started || !profile) {
        return;
    }
    started = false;
    // The game stops sending at once; nobody waits for the answer. Nothing may escape the thread.
    (void)QtConcurrent::run([p = *profile] {
        try {
            OnlineRedirect::NotesAction(p, "stop");
        } catch (...) {
        }
    });
}

void ZeldaNotesDialog::Poll() {
    if (busy || !profile) {
        return;
    }
    busy = true;
    const int call = generation;
    const bool start = !started;
    auto* watcher = new QFutureWatcher<OnlineRedirect::NotesView>(this);
    connect(watcher, &QFutureWatcher<OnlineRedirect::NotesView>::finished, this, [this, watcher, call] {
        const OnlineRedirect::NotesView view = watcher->result();
        watcher->deleteLater();
        if (call != generation) {
            return;
        }
        busy = false;
        Use(view);
    });
    watcher->setFuture(QtConcurrent::run([p = *profile, start] {
        try {
            return start ? OnlineRedirect::NotesAction(p, "start") : OnlineRedirect::FetchNotes(p);
        } catch (...) {
            OnlineRedirect::NotesView view;
            view.error = "the call failed";
            return view;
        }
    }));
}

void ZeldaNotesDialog::Use(const OnlineRedirect::NotesView& view) {
    if (!view.reached || !view.error.empty()) {
        started = false;
        if (view.game == "offline") {
            status->setText(tr("Turn on ZELDA NOTES in Ocarina of Time (title screen > Options > ZELDA "
                               "NOTES) while playing online."));
        } else if (view.game == "other_game") {
            status->setText(tr("The game you're playing online isn't Ocarina of Time."));
        } else {
            status->setText(tr("The server didn't answer: %1").arg(QString::fromStdString(view.error)));
        }
        return;
    }
    started = view.open; // a window the server closed (it went quiet) starts again
    if (!view.open) {
        status->setText(tr("Asking the game again…"));
        return;
    }
    if (!view.map_started) {
        status->setText(tr("Waiting for the game…"));
    } else if (!view.player) {
        status->setText(tr("Connected. Waiting for Link's position…"));
    } else {
        status->setText(tr("Connected to Ocarina of Time."));
    }
    map->Show(view.player);
    if (view.player) {
        const auto& p = *view.player;
        QString text = tr("%1%2 · %3 Link%4")
                           .arg(QString::fromStdString(p.stage.empty() ? std::string("?") : p.stage))
                           .arg(p.zone.empty() ? QString() : QStringLiteral(" / ") + QString::fromStdString(p.zone))
                           .arg(p.child ? tr("Young") : tr("Adult"))
                           .arg(p.indoor ? tr(", indoors") : QString());
        if (p.age > 5) {
            text += tr(" · %n s ago", "", static_cast<int>(p.age));
        }
        place->setText(text);
    } else {
        place->clear();
    }

    collected->clear();
    for (const auto& entry : view.collected) {
        auto* row = new QTreeWidgetItem(collected, {CollectedName(entry.name), QString::number(entry.found)});
        row->setToolTip(1, tr("%1 of the game's %2 flags").arg(entry.found).arg(entry.bits));
    }

    nearby->clear();
    for (const auto& n : view.nearby) {
        new QTreeWidgetItem(
            nearby, {n.type >= 0 ? tr("Type %1").arg(n.type) : tr("Unknown"),
                     n.distance >= 0 ? QString::number(n.distance, 'f', 1) : QString(),
                     n.life >= 0 ? (n.max_life > 0 ? QStringLiteral("%1 / %2").arg(n.life).arg(n.max_life)
                                                   : QString::number(n.life))
                                 : QString(),
                     n.battle ? tr("Yes") : QString()});
    }
    if (view.live == "connected") {
        nearby_note->setText(view.nearby.empty() ? tr("No enemies near Link.") : QString());
    } else if (view.live == "off") {
        nearby_note->setText(tr("The game didn't open its live connection to the server."));
    } else {
        nearby_note->setText(tr("Connecting to the game…"));
    }
}
