// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <thread>

#include <QDialog>
#include <QTimer>

#include "common/common_types.h"

class InputInterpreter;

namespace Core {
class System;
}

namespace Core::HID {
enum class NpadButton : u64;
}

namespace Ui {
class OverlayDialog;
}

/// Whether the in-game overlays (this dialog and the software keyboard) use their dark look: with
/// a dark theme, or a default theme on a dark desktop (as MainWindow::UpdateUITheme picks).
bool UseDarkOverlayTheme();

/// The selection ring of the overlays, after the Nintendo Switch 2's: white next to the selected
/// item, then a band of slowly turning colors. It is drawn over its sibling widgets and lets the
/// mouse through.
class OverlayFocusRing final : public QWidget {
public:
    explicit OverlayFocusRing(QWidget* parent);

    /**
     * Surrounds a rectangle of the parent widget.
     *
     * @param rect The selected item, in the parent's coordinates
     * @param scale Size of the ring: 1 for a 1920x1080 screen
     * @param radius Corner radius of the item
     */
    void Track(const QRect& rect, float scale, qreal radius);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QTimer timer;
    QRect target;
    float scale{1.0f};
    qreal radius{4.0};
    qreal angle{90.0};
};

/**
 * An OverlayDialog is an interactive dialog that accepts controller input (while a game is running)
 * This dialog attempts to replicate the look and feel of the Nintendo Switch's overlay dialogs and
 * provide some extra features such as embedding HTML/Rich Text content in a QTextBrowser.
 * The OverlayDialog provides 2 modes: one to embed regular text into a QLabel and another to embed
 * HTML/Rich Text content into a QTextBrowser.
 */
class OverlayDialog final : public QDialog {
    Q_OBJECT

public:
    explicit OverlayDialog(QWidget* parent, Core::System& system, const QString& title_text,
                           const QString& body_text, const QString& left_button_text,
                           const QString& right_button_text,
                           Qt::Alignment alignment = Qt::AlignCenter, bool use_rich_text_ = false);
    ~OverlayDialog() override;

private:
    /**
     * Initializes a text dialog with a QLabel storing text.
     * Only use this for short text as the dialog buttons would be squashed with longer text.
     *
     * @param title_text Title text to be displayed
     * @param body_text Main text to be displayed
     * @param left_button_text Left button text. If empty, the button is hidden and disabled
     * @param right_button_text Right button text. If empty, the button is hidden and disabled
     * @param alignment Main text alignment
     */
    void InitializeRegularTextDialog(const QString& title_text, const QString& body_text,
                                     const QString& left_button_text,
                                     const QString& right_button_text, Qt::Alignment alignment);

    /**
     * Initializes a text dialog with a QTextBrowser storing text.
     * This is ideal for longer text or rich text content. A scrollbar is shown for longer text.
     *
     * @param title_text Title text to be displayed
     * @param body_text Main text to be displayed
     * @param left_button_text Left button text. If empty, the button is hidden and disabled
     * @param right_button_text Right button text. If empty, the button is hidden and disabled
     * @param alignment Main text alignment
     */
    void InitializeRichTextDialog(const QString& title_text, const QString& body_text,
                                  const QString& left_button_text, const QString& right_button_text,
                                  Qt::Alignment alignment);

    /// Moves and resizes the dialog to be fully overlaid on top of the parent window.
    void MoveAndResizeWindow();

    /**
     * Handles button presses and converts them into keyboard input.
     *
     * @tparam HIDButton The list of buttons that can be converted into keyboard input.
     */
    template <Core::HID::NpadButton... T>
    void HandleButtonPressedOnce();

    /**
     * Translates a button press to focus or click either the left or right buttons.
     *
     * @param button The button press to process.
     */
    void TranslateButtonPress(Core::HID::NpadButton button);

    void StartInputThread();
    void StopInputThread();
    void keyPressEvent(QKeyEvent* e) override;

    /// Keeps the focus ring on the focused button.
    bool eventFilter(QObject* object, QEvent* event) override;

    std::unique_ptr<Ui::OverlayDialog> ui;

    OverlayFocusRing* focus_ring{};

    bool use_rich_text;

    std::unique_ptr<InputInterpreter> input_interpreter;
    std::jthread input_thread;
};
