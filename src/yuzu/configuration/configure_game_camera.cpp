// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>
#include "common/settings.h"
#include "core/hle/service/nx2_camera.h"
#include "yuzu/configuration/configure_game_camera.h"

namespace Nx2Camera = Service::Nx2Camera;

ConfigureGameCamera::ConfigureGameCamera(QWidget* parent) : QWidget(parent) {
    group = new QGroupBox(this);
    enabled = new QCheckBox(group);
    device_label = new QLabel(group);
    device = new QComboBox(group);
    mirror = new QCheckBox(group);
    note = new QLabel(group);
    note->setWordWrap(true);

    auto* const grid = new QGridLayout(group);
    grid->addWidget(enabled, 0, 0, 1, 2);
    grid->addWidget(device_label, 1, 0);
    grid->addWidget(device, 1, 1);
    grid->addWidget(mirror, 2, 0, 1, 2);
    grid->addWidget(note, 3, 0, 1, 2);
    grid->setColumnStretch(1, 1);

    auto* const layout = new QVBoxLayout(this);
    layout->addWidget(group);
    layout->addStretch(1);

    connect(enabled, &QCheckBox::toggled, this, &ConfigureGameCamera::UpdateEnabled);

    RetranslateUI();
    SetConfiguration();
}

ConfigureGameCamera::~ConfigureGameCamera() = default;

void ConfigureGameCamera::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }
    QWidget::changeEvent(event);
}

void ConfigureGameCamera::RetranslateUI() {
    setAccessibleName(tr("Camera"));
    group->setTitle(tr("Camera for games"));
    enabled->setText(tr("Let games use a camera"));
    device_label->setText(tr("Camera:"));
    mirror->setText(tr("Mirror the picture"));
    note->setText(tr("Games such as Mario Kart World show your camera's picture (CameraPlay). The "
                     "camera turns on only while a game uses it, and the status bar shows "
                     "\"Camera on\" then. The picture stays on this computer: Droplet never "
                     "sends it over the network, saves it or writes it to the log. The test "
                     "pattern needs no camera."));
    if (device->count() > 0) {
        device->setItemText(0, tr("Test pattern"));
    }
}

void ConfigureGameCamera::SetConfiguration() {
    enabled->setChecked(Settings::values.camera_enabled.GetValue());
    mirror->setChecked(Settings::values.camera_mirror.GetValue());
    ListCameras();
    UpdateEnabled();
}

// Fills the camera list: the test pattern, then the host's cameras (listed, not turned on). A
// chosen camera that is not plugged in stays in the list so the choice is kept.
void ConfigureGameCamera::ListCameras() {
    const QString chosen = QString::fromStdString(Settings::values.camera_device.GetValue());
    device->clear();
    device->addItem(tr("Test pattern"), QString::fromUtf8(Nx2Camera::TestPatternDevice.data(),
                                                          Nx2Camera::TestPatternDevice.size()));
    for (const auto& name : Nx2Camera::ListDevices()) {
        const QString qname = QString::fromStdString(name);
        device->addItem(qname, qname);
    }
    int index = chosen.isEmpty() ? (device->count() > 1 ? 1 : 0) : device->findData(chosen);
    if (index < 0) {
        device->addItem(tr("%1 (not connected)").arg(chosen), chosen);
        index = device->count() - 1;
    }
    device->setCurrentIndex(index);
}

void ConfigureGameCamera::UpdateEnabled() {
    device->setEnabled(enabled->isChecked());
    device_label->setEnabled(enabled->isChecked());
    mirror->setEnabled(enabled->isChecked());
}

void ConfigureGameCamera::ApplyConfiguration() {
    Settings::values.camera_enabled = enabled->isChecked();
    Settings::values.camera_device = device->currentData().toString().toStdString();
    Settings::values.camera_mirror = mirror->isChecked();
}
