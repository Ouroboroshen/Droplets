// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;

// The Camera tab: lets games such as Mario Kart World use a host camera (Service::Nx2Camera).
// Off by default; listing the cameras here never turns one on.
class ConfigureGameCamera : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureGameCamera(QWidget* parent = nullptr);
    ~ConfigureGameCamera() override;

    void ApplyConfiguration();

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();
    void SetConfiguration();
    void ListCameras();
    void UpdateEnabled();

    QGroupBox* group;
    QCheckBox* enabled;
    QLabel* device_label;
    QComboBox* device;
    QCheckBox* mirror;
    QLabel* note;
};
