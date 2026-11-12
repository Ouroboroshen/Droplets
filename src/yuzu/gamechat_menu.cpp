// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include "common/settings.h"
#include "core/hle/service/nx2_camera.h"
#include "core/hle/service/nx2_chat.h"
#include "yuzu/gamechat_menu.h"

namespace Nx2Chat = Service::Nx2Chat;

namespace {

QString FormatCode(const std::string& code) {
    const QString digits = QString::fromStdString(code);
    return digits.size() == 12 ? QStringLiteral("SW-%1-%2-%3")
                                     .arg(digits.mid(0, 4), digits.mid(4, 4), digits.mid(8, 4))
                               : digits;
}

// A player as the lists show them: their name in the game, else their friend code. Plain text:
// names come from other players.
QString PlayerName(const std::string& name, const std::string& code) {
    return name.empty() ? FormatCode(code) : QString::fromStdString(name);
}

constexpr int CodeRole = Qt::UserRole;

} // namespace

GameChatMenu::GameChatMenu(QWidget* parent) : QDialog(parent) {
    status = new QLabel(this);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    members_label = new QLabel(this);
    members = new QListWidget(this);
    members->setSelectionMode(QAbstractItemView::SingleSelection);
    calls = new QLabel(this);
    calls->setWordWrap(true);
    calls->setTextFormat(Qt::PlainText);
    volume_label = new QLabel(this);
    volume_label->setTextFormat(Qt::PlainText);
    volume = new QSlider(Qt::Horizontal, this);
    volume->setRange(0, 200);
    volume->setSingleStep(10);
    volume->setPageStep(25);
    volume_value = new QLabel(this);
    volume_value->setMinimumWidth(volume_value->fontMetrics().horizontalAdvance(QStringLiteral("200%")));
    mute = new QPushButton(this);
    voice = new QLabel(this);
    voice->setWordWrap(true);
    voice->setTextFormat(Qt::PlainText);
    start = new QPushButton(this);
    leave = new QPushButton(this);
    friends_label = new QLabel(this);
    friends = new QListWidget(this);
    friends->setSelectionMode(QAbstractItemView::NoSelection);
    invite = new QPushButton(this);
    invites_label = new QLabel(this);
    invites = new QListWidget(this);
    join = new QPushButton(this);
    decline = new QPushButton(this);
    share = new QCheckBox(this);
    camera = new QLabel(this);
    camera->setWordWrap(true);
    camera->setTextFormat(Qt::PlainText);
    join_muted = new QCheckBox(this);
    lower_game = new QCheckBox(this);
    microphone = new QLabel(this);
    microphone->setWordWrap(true);
    microphone->setTextFormat(Qt::PlainText);
    privacy = new QLabel(this);
    privacy->setWordWrap(true);
    close = new QPushButton(this);
    for (auto* list : {members, friends, invites}) {
        list->setMaximumHeight(120);
    }
    setObjectName(QStringLiteral("gamechat_menu"));
    members->setObjectName(QStringLiteral("gamechat_members"));
    friends->setObjectName(QStringLiteral("gamechat_friends"));
    invites->setObjectName(QStringLiteral("gamechat_invites"));
    share->setObjectName(QStringLiteral("gamechat_share"));
    volume->setObjectName(QStringLiteral("gamechat_volume"));
    mute->setObjectName(QStringLiteral("gamechat_mute"));
    start->setObjectName(QStringLiteral("gamechat_start"));
    leave->setObjectName(QStringLiteral("gamechat_leave"));
    invite->setObjectName(QStringLiteral("gamechat_call"));
    join->setObjectName(QStringLiteral("gamechat_join"));
    decline->setObjectName(QStringLiteral("gamechat_decline"));
    join_muted->setObjectName(QStringLiteral("gamechat_join_muted"));
    lower_game->setObjectName(QStringLiteral("gamechat_lower_game"));

    auto* const volume_row = new QHBoxLayout;
    volume_row->addWidget(volume_label);
    volume_row->addWidget(volume, 1);
    volume_row->addWidget(volume_value);
    auto* const voice_row = new QHBoxLayout;
    voice_row->addWidget(mute);
    voice_row->addWidget(voice, 1);
    auto* const room_buttons = new QHBoxLayout;
    room_buttons->addWidget(leave);
    room_buttons->addStretch(1);
    auto* const call_buttons = new QHBoxLayout;
    call_buttons->addWidget(start);
    call_buttons->addWidget(invite);
    call_buttons->addStretch(1);
    auto* const answer_buttons = new QHBoxLayout;
    answer_buttons->addWidget(join);
    answer_buttons->addWidget(decline);
    answer_buttons->addStretch(1);
    auto* const bottom = new QHBoxLayout;
    bottom->addStretch(1);
    bottom->addWidget(close);

    auto* const layout = new QVBoxLayout(this);
    layout->addWidget(status);
    layout->addWidget(members_label);
    layout->addWidget(members);
    layout->addWidget(calls);
    layout->addLayout(volume_row);
    layout->addLayout(voice_row);
    layout->addLayout(room_buttons);
    layout->addWidget(friends_label);
    layout->addWidget(friends);
    layout->addLayout(call_buttons);
    layout->addWidget(invites_label);
    layout->addWidget(invites);
    layout->addLayout(answer_buttons);
    layout->addSpacing(8);
    layout->addWidget(share);
    layout->addWidget(camera);
    layout->addWidget(join_muted);
    layout->addWidget(lower_game);
    layout->addWidget(microphone);
    layout->addWidget(privacy);
    layout->addLayout(bottom);
    setMinimumWidth(460);

    refresh_timer = new QTimer(this);
    refresh_timer->setInterval(3000);
    connect(refresh_timer, &QTimer::timeout, this, [] { Nx2Chat::Refresh(); });
    connect(start, &QPushButton::clicked, this, &GameChatMenu::CallTicked);
    connect(invite, &QPushButton::clicked, this, &GameChatMenu::CallTicked);
    connect(leave, &QPushButton::clicked, this, [] { Nx2Chat::Leave(); });
    connect(mute, &QPushButton::clicked, this, [this] { Nx2Chat::SetMuted(!muted); });
    connect(join, &QPushButton::clicked, this, [this] {
        if (const auto* item = invites->currentItem()) {
            Nx2Chat::Join(item->data(CodeRole).toString().toStdString());
        }
    });
    connect(decline, &QPushButton::clicked, this, [this] {
        if (const auto* item = invites->currentItem()) {
            Nx2Chat::Decline(item->data(CodeRole).toString().toStdString());
        }
    });
    // The lists are made again by UpdateView, so never inside their own signals
    connect(members, &QListWidget::currentItemChanged, this, [this] { UpdateView(); },
            Qt::QueuedConnection);
    connect(volume, &QSlider::valueChanged, this, [this](int value) {
        volume_value->setText(tr("%1%").arg(value));
        if (const auto* item = members->currentItem()) {
            Nx2Chat::SetMemberVolume(item->data(CodeRole).toString().toStdString(), value);
        }
    });
    connect(friends, &QListWidget::itemChanged, this, [this] { UpdateView(); },
            Qt::QueuedConnection);
    connect(invites, &QListWidget::currentItemChanged, this, [this] { UpdateView(); },
            Qt::QueuedConnection);
    connect(share, &QCheckBox::toggled, this, &GameChatMenu::OnShareToggled);
    connect(join_muted, &QCheckBox::toggled, this, [this](bool checked) {
        Settings::values.gamechat_join_muted = checked;
        emit SettingsChanged();
    });
    connect(lower_game, &QCheckBox::toggled, this, [this](bool checked) {
        Settings::values.gamechat_lower_game_volume = checked;
        emit SettingsChanged();
    });
    connect(close, &QPushButton::clicked, this, &QDialog::hide);

    RetranslateUI();
    UpdateView();
}

GameChatMenu::~GameChatMenu() = default;

void GameChatMenu::showEvent(QShowEvent* event) {
    // The player sees the calls now: they stop ringing
    Nx2Chat::MenuShown();
    Nx2Chat::Refresh();
    refresh_timer->start();
    UpdateView();
    QDialog::showEvent(event);
}

void GameChatMenu::hideEvent(QHideEvent* event) {
    refresh_timer->stop();
    QDialog::hideEvent(event);
}

void GameChatMenu::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
        UpdateView();
    }
    QDialog::changeEvent(event);
}

void GameChatMenu::RetranslateUI() {
    setWindowTitle(tr("C Menu: GameChat"));
    members_label->setText(tr("In this GameChat:"));
    start->setText(tr("Start GameChat"));
    start->setToolTip(tr("Starts a GameChat and calls the friends you ticked."));
    leave->setText(tr("Leave GameChat"));
    invite->setText(tr("Call"));
    invite->setToolTip(tr("Calls the friends you ticked into this GameChat."));
    invites_label->setText(tr("Calls:"));
    join->setText(tr("Join"));
    decline->setText(tr("Decline"));
    share->setText(tr("Show my camera to the people in my GameChat"));
    join_muted->setText(tr("Join GameChats with my microphone muted"));
    lower_game->setText(tr("Lower the game's sound while others talk"));
    privacy->setText(tr("Your voice and picture go through your online server only to the people "
                        "in this GameChat: your voice while your microphone is on, your picture "
                        "while a game shows it there. Droplet never records or saves them, and "
                        "Reservoir servers pass them on without keeping them. Use headphones: "
                        "Droplet doesn't cancel echo."));
    close->setText(tr("Close"));
}

void GameChatMenu::UpdateCameraText(bool sharing) {
    if (!Settings::values.camera_enabled.GetValue()) {
        camera->setText(tr("Your camera is off. To share it, turn it on in the Camera menu (Let "
                           "Games Use a Camera)."));
        return;
    }
    const std::string device = Settings::values.camera_device.GetValue();
    const QString name = device == Service::Nx2Camera::TestPatternDevice
                             ? tr("Test pattern")
                             : QString::fromStdString(device);
    QString text = tr("Camera: %1. Choose another in the Camera menu.").arg(name);
    if (sharing) {
        text += QLatin1Char(' ') + tr("The people in your GameChat see it now.");
    }
    camera->setText(text);
}

// The microphone button and what the GameChat's voice is doing.
void GameChatMenu::UpdateVoice(const Service::Nx2Chat::View& view, bool in_room) {
    muted = view.muted;
    mute->setText(view.muted ? tr("Unmute Microphone") : tr("Mute Microphone"));
    mute->setVisible(in_room);
    mute->setEnabled(view.voice != Nx2Chat::VoiceState::NoVoice);
    voice->setVisible(in_room);
    using Nx2Chat::VoiceState;
    if (view.voice == VoiceState::NoVoice) {
        voice->setText(tr("This server has no GameChat voice (an older Reservoir): nobody here can "
                          "talk or listen."));
    } else if (view.voice == VoiceState::Failed) {
        voice->setText(tr("Voice: %1. Trying again.").arg(QString::fromStdString(view.voice_error)));
    } else if (view.voice != VoiceState::On) {
        voice->setText(tr("Connecting to the GameChat's voice..."));
    } else if (view.muted) {
        voice->setText(tr("Your microphone is muted."));
    } else if (view.no_microphone) {
        voice->setText(tr("No microphone chosen: you hear the others, but they can't hear you."));
    } else if (view.microphone_failed) {
        voice->setText(tr("Your microphone didn't open. Check Configure > Audio > Input Device, "
                          "then unmute again."));
    } else if (view.microphone) {
        voice->setText(tr("Your microphone is on: the people in this GameChat hear you."));
    } else {
        voice->setText(tr("Turning your microphone on..."));
    }

    const std::string device = Settings::values.audio_input_device_id.GetValue();
    if (view.no_microphone) {
        microphone->setText(tr("To talk, choose a microphone in Configure > Audio > Input Device."));
    } else {
        const QString name = device == "auto" ? tr("the default microphone")
                                              : QString::fromStdString(device);
        microphone->setText(
            tr("Microphone: %1. Choose another in Configure > Audio > Input Device.").arg(name));
    }
}

void GameChatMenu::UpdateView() {
    const auto view = Nx2Chat::Snapshot();
    const bool in_room = !view.room.empty();
    const bool online = !view.server.empty();
    const bool voice_on = view.voice == Nx2Chat::VoiceState::On;
    if (!online) {
        status->setText(tr("GameChat needs online play on a server: turn it on in Multiplayer > "
                           "Online Play Settings."));
    } else if (!view.error.empty()) {
        status->setText(tr("GameChat: %1").arg(QString::fromStdString(view.error)));
    } else if (in_room) {
        status->setText(tr("You're in a GameChat (%1 of %2 people). Talk with the people in it; "
                           "games such as Mario Kart World show the faces of those who share "
                           "their camera.")
                            .arg(view.members.size())
                            .arg(std::max<size_t>(view.max_members, view.members.size())));
    } else {
        status->setText(tr("You're not in a GameChat. Tick friends and start one to call them, or "
                           "answer a friend's call."));
    }

    // The members, with their microphones (this player's as it is here), who talks and cameras
    const QString picked_member =
        members->currentItem() ? members->currentItem()->data(CodeRole).toString() : QString{};
    members->blockSignals(true);
    members->clear();
    const Nx2Chat::Member* picked = nullptr;
    for (const auto& member : view.members) {
        QStringList states;
        if (voice_on) {
            using Nx2Chat::MicState;
            if (member.self) {
                states << (view.muted                ? tr("muted")
                           : view.no_microphone      ? tr("no microphone")
                           : view.microphone_failed  ? tr("microphone didn't open")
                                                     : tr("microphone on"));
            } else {
                states << (member.microphone == MicState::On      ? tr("microphone on")
                           : member.microphone == MicState::Muted ? tr("muted")
                                                                  : tr("no microphone"));
            }
            if (member.talking) {
                states << tr("talking");
            }
        }
        if (member.camera) {
            states << tr("camera on");
        }
        if (!member.self && member.volume != 100) {
            states << tr("volume %1%").arg(member.volume);
        }
        QString text = PlayerName(member.name, member.code);
        if (member.self) {
            text = tr("%1 (you)").arg(text);
        }
        if (!states.isEmpty()) {
            text = tr("%1: %2").arg(text, states.join(QStringLiteral(", ")));
        }
        auto* const item = new QListWidgetItem(text, members);
        item->setData(CodeRole, QString::fromStdString(member.code));
        if (member.self) {
            item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        } else if (item->data(CodeRole).toString() == picked_member) {
            members->setCurrentItem(item);
            picked = &member;
        }
    }
    members->blockSignals(false);
    members_label->setVisible(in_room);
    members->setVisible(in_room);

    // Calls this GameChat made that haven't been answered
    QStringList calling;
    QStringList declined;
    for (const auto& call : view.calls) {
        (call.declined ? declined : calling) << PlayerName(call.name, call.code);
    }
    QString calls_text;
    if (!calling.isEmpty()) {
        calls_text = tr("Calling %1...").arg(calling.join(QStringLiteral(", ")));
    }
    if (!declined.isEmpty()) {
        calls_text += (calls_text.isEmpty() ? QString{} : QStringLiteral(" ")) +
                      tr("%1 declined.").arg(declined.join(QStringLiteral(", ")));
    }
    calls->setText(calls_text);
    calls->setVisible(in_room && !calls_text.isEmpty());

    // How loud the picked member is
    const bool show_volume = in_room && picked != nullptr && voice_on;
    volume_label->setVisible(show_volume);
    volume->setVisible(show_volume);
    volume_value->setVisible(show_volume);
    if (show_volume) {
        volume_label->setText(tr("%1's voice:").arg(PlayerName(picked->name, picked->code)));
        volume->blockSignals(true);
        volume->setValue(picked->volume);
        volume->blockSignals(false);
        volume_value->setText(tr("%1%").arg(picked->volume));
    }
    UpdateVoice(view, in_room);

    start->setVisible(!in_room);
    start->setEnabled(online && !view.busy);
    leave->setVisible(in_room);
    leave->setEnabled(!view.busy);

    // Friends not in the GameChat yet, keeping the ticks
    QStringList ticked;
    for (int i = 0; i < friends->count(); ++i) {
        if (friends->item(i)->checkState() == Qt::Checked) {
            ticked << friends->item(i)->data(CodeRole).toString();
        }
    }
    friends->blockSignals(true);
    friends->clear();
    int ticked_count = 0;
    for (const auto& entry : view.friends) {
        const bool member = std::any_of(view.members.begin(), view.members.end(),
                                        [&](const auto& m) { return m.code == entry.code; });
        if (member) {
            continue;
        }
        QString text = entry.online ? tr("%1, online").arg(PlayerName(entry.name, entry.code))
                                    : PlayerName(entry.name, entry.code);
        const bool being_called = std::any_of(view.calls.begin(), view.calls.end(),
                                              [&](const auto& c) {
                                                  return c.code == entry.code && !c.declined;
                                              });
        if (being_called) {
            text = tr("%1, calling").arg(text);
        }
        auto* const item = new QListWidgetItem(text, friends);
        item->setData(CodeRole, QString::fromStdString(entry.code));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        const bool tick = ticked.contains(item->data(CodeRole).toString());
        item->setCheckState(tick ? Qt::Checked : Qt::Unchecked);
        ticked_count += tick ? 1 : 0;
    }
    friends->blockSignals(false);
    const size_t room_left = view.max_members == 0 || view.members.size() >= view.max_members
                                 ? 0
                                 : view.max_members - view.members.size();
    const bool can_call = friends->count() > 0;
    friends_label->setVisible(online);
    friends->setVisible(online && can_call);
    invite->setVisible(in_room && can_call);
    invite->setEnabled(ticked_count > 0 && room_left > 0 && !view.busy);
    if (view.friends.empty()) {
        friends_label->setText(tr("To call friends, add them in Multiplayer > Online Play "
                                  "Settings."));
    } else if (!can_call) {
        friends_label->setText(tr("Your friends are all in this GameChat."));
    } else {
        friends_label->setText(in_room ? tr("Call more friends:")
                                       : tr("Call friends into a new GameChat:"));
    }

    const QString picked_invite =
        invites->currentItem() ? invites->currentItem()->data(CodeRole).toString() : QString{};
    invites->blockSignals(true);
    invites->clear();
    for (const auto& entry : view.invites) {
        const QString from = PlayerName(entry.name, entry.code);
        auto* const item = new QListWidgetItem(
            entry.ringing ? tr("%1 is calling you").arg(from) : tr("%1 called you").arg(from),
            invites);
        item->setData(CodeRole, QString::fromStdString(entry.room));
        if (item->data(CodeRole).toString() == picked_invite) {
            invites->setCurrentItem(item);
        }
    }
    if (invites->currentItem() == nullptr && invites->count() > 0) {
        invites->setCurrentRow(0);
    }
    invites->blockSignals(false);
    const bool any_invites = !view.invites.empty();
    invites_label->setVisible(any_invites);
    invites->setVisible(any_invites);
    join->setVisible(any_invites);
    decline->setVisible(any_invites);
    join->setEnabled(invites->currentItem() != nullptr && !view.busy);
    decline->setEnabled(invites->currentItem() != nullptr && !view.busy);

    for (auto* box : {share, join_muted, lower_game}) {
        box->blockSignals(true);
    }
    share->setChecked(Settings::values.gamechat_share_camera.GetValue());
    join_muted->setChecked(Settings::values.gamechat_join_muted.GetValue());
    lower_game->setChecked(Settings::values.gamechat_lower_game_volume.GetValue());
    for (auto* box : {share, join_muted, lower_game}) {
        box->blockSignals(false);
    }
    UpdateCameraText(view.sharing);

    // Fit the window to the parts shown, once they change
    const int sections = (in_room ? 1 : 0) | (any_invites ? 2 : 0) | (can_call ? 4 : 0) |
                         (show_volume ? 8 : 0) | (calls_text.isEmpty() ? 0 : 16);
    if (sections != shown_sections) {
        shown_sections = sections;
        QTimer::singleShot(0, this, [this] { adjustSize(); });
    }
}

void GameChatMenu::OnShareToggled(bool checked) {
    Settings::values.gamechat_share_camera = checked;
    emit SettingsChanged();
    UpdateView();
}

// Calls the ticked friends: into this GameChat, or a new one when not in one.
void GameChatMenu::CallTicked() {
    std::vector<std::string> codes;
    for (int i = 0; i < friends->count(); ++i) {
        auto* const item = friends->item(i);
        if (item->checkState() == Qt::Checked) {
            codes.push_back(item->data(CodeRole).toString().toStdString());
        }
    }
    friends->blockSignals(true);
    for (int i = 0; i < friends->count(); ++i) {
        friends->item(i)->setCheckState(Qt::Unchecked);
    }
    friends->blockSignals(false);
    Nx2Chat::CallFriends(codes);
    UpdateView();
}
