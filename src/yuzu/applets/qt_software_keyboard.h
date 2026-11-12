// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <thread>
#include <vector>

#include <QDialog>
#include <QString>

#include "core/frontend/applets/software_keyboard.h"

class InputInterpreter;
class OverlayFocusRing;

namespace Core {
class System;
}

namespace Core::HID {
enum class NpadButton : u64;
}

namespace QtSoftwareKeyboardWidgets {
class Key;
class LineEdit;
class Panel;
class TextEdit;
struct Theme;
} // namespace QtSoftwareKeyboardWidgets

class MainWindow;

/**
 * The software keyboard, drawn after the Nintendo Switch 2's system keyboard: white (or, with a
 * dark theme, gray) rounded keys on a panel at the bottom of the game screen, a ring around the
 * key the controller is on, and the button guide below. Everything is painted with Qt; no
 * console images, fonts or sounds are used.
 */
class QtSoftwareKeyboardDialog final : public QDialog {
    Q_OBJECT

public:
    QtSoftwareKeyboardDialog(QWidget* parent, Core::System& system_, bool is_inline_,
                             Core::Frontend::KeyboardInitializeParameters initialize_parameters_);
    ~QtSoftwareKeyboardDialog() override;

    void ShowNormalKeyboard(QPoint pos, QSize size);

    void ShowTextCheckDialog(Service::AM::Frontend::SwkbdTextCheckResult text_check_result,
                             std::u16string text_check_message);

    void ShowInlineKeyboard(Core::Frontend::InlineAppearParameters appear_parameters, QPoint pos,
                            QSize size);

    void HideInlineKeyboard();

    void InlineTextChanged(Core::Frontend::InlineTextParameters text_parameters);

    void ExitKeyboard();

    /// Droplet test hook (EDEN_NX2_QT_STEPS oskpad in main_window.cpp): a controller button press.
    void PressButtonForTest(Core::HID::NpadButton button);

signals:
    void SubmitNormalText(Service::AM::Frontend::SwkbdResult result, std::u16string submitted_text,
                          bool confirmed = false) const;

    void SubmitInlineText(Service::AM::Frontend::SwkbdReplyType reply_type,
                          std::u16string submitted_text, s32 cursor_position) const;

public slots:
    void open() override;
    void reject() override;

protected:
    /// We override the keyPressEvent for inputting text into the inline software keyboard.
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    using Key = QtSoftwareKeyboardWidgets::Key;

    enum class Direction {
        Left,
        Up,
        Right,
        Down,
    };

    /// The keys on screen. The letter, symbol and accent pages share one grid of keys.
    enum class Page {
        Letters,
        Symbols,
        Vowels,
        Consonants,
        NumberPad,
    };

    /// Creates every key of the keyboard and the navigation grids.
    void CreateKeys();

    /**
     * Moves and resizes the window to a specified position and size.
     *
     * @param pos Top-left window position
     * @param size Window size
     */
    void MoveAndResizeWindow(QPoint pos, QSize size);

    /// Places the text area, the panel and the keys for the current size and page.
    void LayoutKeyboard();

    /// Sets the keyboard type based on initialize_parameters.
    void SetKeyboardType();

    /// Sets the password mode based on initialize_parameters.
    void SetPasswordMode();

    /// Sets the text draw type based on initialize_parameters.
    void SetTextDrawType();

    /// Sets the controller image at the bottom left of the software keyboard.
    void SetControllerImage();

    /// Disables buttons based on initialize_parameters.
    void DisableKeyboardButtons();

    /// Enables the character keys whose characters the game allows.
    void UpdateCharacterKeys();

    /// Changes whether the backspace or/and ok buttons should be enabled or disabled.
    void SetBackspaceOkEnabled();

    /// Whether the game allows this character to be entered from the keys.
    bool IsCharacterAllowed(QChar character) const;

    /**
     * Validates the input text sent in based on the parameters in initialize_parameters.
     *
     * @param input_text Input text
     *
     * @returns True if the input text is valid, false otherwise.
     */
    bool ValidateInputText(const QString& input_text);

    /// Shows a page of keys (the tabs at the bottom left).
    void SetPage(Page new_page);

    /// Switches between lower case and upper case (Shift and Caps Lock).
    void ChangeShift();

    /// Writes the current page's characters on the character keys.
    void UpdateKeyLabels();

    /// Processes a keyboard button click from the UI as normal keyboard input.
    void NormalKeyboardButtonClicked(Key* key);

    /// Processes a keyboard button click from the UI as inline keyboard input.
    void InlineKeyboardButtonClicked(Key* key);

    /**
     * Inserts a string of arbitrary length into the current_text at the current cursor position.
     * This is only used for the inline software keyboard.
     */
    void InlineTextInsertString(std::u16string_view string);

    /// The mouse moved onto a key: it becomes the selected key, and the controller ring hides.
    void KeyHovered(Key* key);

    /**
     * Handles button presses and converts them into keyboard input.
     *
     * @tparam HIDButton The list of buttons that can be converted into keyboard input.
     */
    template <Core::HID::NpadButton... T>
    void HandleButtonPressedOnce();

    /**
     * Handles button holds and converts them into keyboard input.
     *
     * @tparam HIDButton The list of buttons that can be converted into keyboard input.
     */
    template <Core::HID::NpadButton... T>
    void HandleButtonHold();

    /**
     * Translates a button press to focus or click a keyboard button.
     *
     * @param button The button press to process.
     */
    void TranslateButtonPress(Core::HID::NpadButton button);

    /**
     * Moves the focus of a button in a certain direction.
     *
     * @param direction The direction to move.
     */
    void MoveButtonDirection(Direction direction);

    /**
     * Moves the text cursor in a certain direction.
     *
     * @param direction The direction to move.
     */
    void MoveTextCursorDirection(Direction direction);

    /// The key at (row, column) of the current grid.
    Key* GridKey(std::size_t grid_row, std::size_t grid_column) const;

    /// Moves the controller ring to the selected key.
    void UpdateFocusRing();

    /// Updates the button guide below the keys (Shift label, L and R).
    void UpdateLegend();

    /// The text being edited and the text cursor, in any mode.
    QString CurrentText() const;
    int CurrentCursorPosition() const;

    /// The character count below the text field.
    void UpdateCharacterCount();

    void StartInputThread();
    void StopInputThread();

    Core::System& system;

    // True if it is the inline software keyboard.
    bool is_inline;

    // Common software keyboard initialize parameters.
    Core::Frontend::KeyboardInitializeParameters initialize_parameters;

    // Used only by the inline software keyboard since the QLineEdit or QTextEdit is hidden.
    std::u16string current_text;
    s32 cursor_position{0};

    std::unique_ptr<QtSoftwareKeyboardWidgets::Theme> theme;
    QtSoftwareKeyboardWidgets::Panel* panel{};
    OverlayFocusRing* focus_ring{};
    QtSoftwareKeyboardWidgets::LineEdit* line_edit{};
    QtSoftwareKeyboardWidgets::TextEdit* text_edit{};

    // The text area of the normal keyboard (in Line mode the underline, in Box mode the box) and
    // the character count below it.
    QRect text_field_rect;
    QRect character_count_rect;
    QRect header_rect;
    QRect sub_rect;
    QString character_count;
    bool box_mode{false};
    bool wide_line{true};

    static constexpr std::size_t NUM_ROWS_NORMAL = 5;
    static constexpr std::size_t NUM_COLUMNS_NORMAL = 12;
    static constexpr std::size_t NUM_ROWS_NUMPAD = 4;
    static constexpr std::size_t NUM_COLUMNS_NUMPAD = 4;
    static constexpr std::size_t NUM_CHARACTER_ROWS = 4;
    static constexpr std::size_t NUM_CHARACTER_COLUMNS = 11;

    // The 44 keys that type characters on the letter, symbol and accent pages.
    std::array<std::array<Key*, NUM_CHARACTER_COLUMNS>, NUM_CHARACTER_ROWS> character_keys{};
    Key* key_backspace{};
    Key* key_return{};
    Key* key_ok{};
    Key* key_globe{};
    Key* key_shift{};
    Key* key_space{};
    // ABC, #+=, ÀÁÂ and ÑŃŅ.
    std::array<Key*, 4> tab_keys{};

    std::array<Key*, 10> number_keys{};
    Key* key_left_optional{};
    Key* key_right_optional{};
    Key* key_backspace_num{};
    Key* key_ok_num{};

    // The navigation grids: keys that span several cells appear in each of them.
    std::array<std::array<Key*, NUM_COLUMNS_NORMAL>, NUM_ROWS_NORMAL> keyboard_buttons{};
    std::array<std::array<Key*, NUM_COLUMNS_NUMPAD>, NUM_ROWS_NUMPAD> numberpad_buttons{};

    std::vector<Key*> all_keys;

    std::size_t row{0};
    std::size_t column{0};

    Page page{Page::Letters};
    // Qwerty keyboards (passwords, addresses) and download codes have their own letter rows and
    // only the ABC and #+= pages.
    bool url_layout{false};
    bool shift_enabled{false};
    bool caps_lock_enabled{false};

    std::unique_ptr<InputInterpreter> input_interpreter;
    std::jthread input_thread;
};

class QtSoftwareKeyboard final : public QObject, public Core::Frontend::SoftwareKeyboardApplet {
    Q_OBJECT

public:
    explicit QtSoftwareKeyboard(MainWindow& parent);
    ~QtSoftwareKeyboard() override;

    void Close() const override {
        ExitKeyboard();
    }

    void InitializeKeyboard(bool is_inline,
                            Core::Frontend::KeyboardInitializeParameters initialize_parameters,
                            SubmitNormalCallback submit_normal_callback_,
                            SubmitInlineCallback submit_inline_callback_) override;

    void ShowNormalKeyboard() const override;

    void ShowTextCheckDialog(Service::AM::Frontend::SwkbdTextCheckResult text_check_result,
                             std::u16string text_check_message) const override;

    void ShowInlineKeyboard(
        Core::Frontend::InlineAppearParameters appear_parameters) const override;

    void HideInlineKeyboard() const override;

    void InlineTextChanged(Core::Frontend::InlineTextParameters text_parameters) const override;

    void ExitKeyboard() const override;

signals:
    void MainWindowInitializeKeyboard(
        bool is_inline, Core::Frontend::KeyboardInitializeParameters initialize_parameters) const;

    void MainWindowShowNormalKeyboard() const;

    void MainWindowShowTextCheckDialog(
        Service::AM::Frontend::SwkbdTextCheckResult text_check_result,
        std::u16string text_check_message) const;

    void MainWindowShowInlineKeyboard(
        Core::Frontend::InlineAppearParameters appear_parameters) const;

    void MainWindowHideInlineKeyboard() const;

    void MainWindowInlineTextChanged(Core::Frontend::InlineTextParameters text_parameters) const;

    void MainWindowExitKeyboard() const;

private:
    void SubmitNormalText(Service::AM::Frontend::SwkbdResult result, std::u16string submitted_text,
                          bool confirmed) const;

    void SubmitInlineText(Service::AM::Frontend::SwkbdReplyType reply_type,
                          std::u16string submitted_text, s32 cursor_position) const;

    mutable SubmitNormalCallback submit_normal_callback;
    mutable SubmitInlineCallback submit_inline_callback;
};
