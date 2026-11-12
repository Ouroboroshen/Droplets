// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>

#include <QConicalGradient>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QStyleHints>
#include <QWindow>

#include "core/core.h"
#include "hid_core/frontend/input_interpreter.h"
#include "hid_core/hid_types.h"
#include "qt_common/config/uisettings.h"
#include "ui_overlay_dialog.h"
#include "yuzu/util/overlay_dialog.h"

namespace {

constexpr float BASE_TITLE_FONT_SIZE = 14.0f;
constexpr float BASE_FONT_SIZE = 18.0f;
constexpr float BASE_WIDTH = 1280.0f;
constexpr float BASE_HEIGHT = 720.0f;

struct OverlayPalette {
    const char* dim;
    const char* box;
    const char* divider;
    const char* title;
    const char* text;
    const char* button_text;
    const char* selected_button;
    const char* pressed_button;
    const char* scroll_bar;
    const char* scroll_handle;
    const char* disabled_text;
};

constexpr OverlayPalette LIGHT_PALETTE{
    .dim = "rgba(0, 0, 0, 89)",
    .box = "#eeeeee",
    .divider = "#cbcbcb",
    .title = "#6e6e6e",
    .text = "#1f1f1f",
    .button_text = "#0065d6",
    .selected_button = "#ffffff",
    .pressed_button = "#d2d2d2",
    .scroll_bar = "#d8d8d8",
    .scroll_handle = "#ffffff",
    .disabled_text = "#bfbfbf",
};
constexpr OverlayPalette DARK_PALETTE{
    .dim = "rgba(0, 0, 0, 150)",
    .box = "#2e2e2e",
    .divider = "#4a4a4a",
    .title = "#a6a6a6",
    .text = "#eeeeee",
    .button_text = "#3cb0e4",
    .selected_button = "#454545",
    .pressed_button = "#5a5a5a",
    .scroll_bar = "#4a4a4a",
    .scroll_handle = "#8a8a8a",
    .disabled_text = "#6b6b6b",
};

// The Nintendo Switch 2's message boxes: a light (or dark) rounded box on the dimmed screen,
// blue button text, and the selected button white (OverlayFocusRing draws its ring).
QString OverlayStyleSheet(const OverlayPalette& palette) {
    return QStringLiteral(
               "QDialog#OverlayDialog, QStackedWidget#stackedDialog { background: %1; }"
               "QWidget#lineDialog, QWidget#richDialog { background: transparent; }"
               "QWidget#contentDialog, QWidget#contentRichDialog { background: %2; margin: 5px;"
               " border-radius: 14px; }"
               "QWidget#buttonsDialog, QWidget#buttonsRichDialog { background: transparent;"
               " margin: 5px; border: none; border-top: 2px solid %3; }"
               "QLabel#label_title, QLabel#label_title_rich { background: transparent; color: %4;"
               " padding: 0px 65px; }"
               "QLabel#label_dialog { background: transparent; color: %5; padding: 20px 65px; }"
               "QTextBrowser#text_browser_dialog { background: transparent; color: %5;"
               " border: none; padding: 35px 65px; }"
               "QDialog#OverlayDialog QPushButton { color: %6; background: transparent;"
               " border: none; padding: 0px; min-width: 0px; }"
               "QDialog#OverlayDialog QPushButton:focus, QDialog#OverlayDialog QPushButton:hover {"
               " background: %7; border: none; border-radius: 12px; outline: none; }"
               "QDialog#OverlayDialog QPushButton:pressed { background: %8; }"
               "QDialog#OverlayDialog QPushButton:disabled { color: %11; }"
               "QTextBrowser#text_browser_dialog QScrollBar:vertical { background: %9;"
               " width: 15px; margin: 15px 3px 15px 3px; border: 1px transparent;"
               " border-radius: 4px; }"
               "QTextBrowser#text_browser_dialog QScrollBar::handle { background: %10;"
               " border-radius: 4px; min-height: 5px; min-width: 5px; }"
               "QTextBrowser#text_browser_dialog QScrollBar::add-line,"
               " QTextBrowser#text_browser_dialog QScrollBar::sub-line,"
               " QTextBrowser#text_browser_dialog QScrollBar::add-page,"
               " QTextBrowser#text_browser_dialog QScrollBar::sub-page { background: none; }")
        .arg(QLatin1String(palette.dim), QLatin1String(palette.box), QLatin1String(palette.divider),
             QLatin1String(palette.title), QLatin1String(palette.text),
             QLatin1String(palette.button_text), QLatin1String(palette.selected_button),
             QLatin1String(palette.pressed_button), QLatin1String(palette.scroll_bar),
             QLatin1String(palette.scroll_handle), QLatin1String(palette.disabled_text));
}

// Corner radius of the buttons in the style sheet.
constexpr qreal BUTTON_RADIUS = 12.0;

} // Anonymous namespace

bool UseDarkOverlayTheme() {
    if (UISettings::IsDarkTheme()) {
        return true;
    }
    const std::string& theme = UISettings::values.theme;
    if (!theme.empty() && theme != "default" && theme != "colorful") {
        return false;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
    return false;
#endif
}

OverlayFocusRing::OverlayFocusRing(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    // Over the app's style, which may give every widget a background.
    setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    timer.setInterval(33);
    connect(&timer, &QTimer::timeout, this, [this] {
        angle = std::fmod(angle + 3.0, 360.0);
        update();
    });
}

void OverlayFocusRing::Track(const QRect& rect, float scale_, qreal radius_) {
    target = rect;
    scale = scale_;
    radius = radius_;
    const int margin = static_cast<int>(std::ceil(13.0 * scale)) + 1;
    setGeometry(target.adjusted(-margin, -margin, margin, margin));
    update();
}

void OverlayFocusRing::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    timer.start();
}

void OverlayFocusRing::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    timer.stop();
}

void OverlayFocusRing::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF item = QRectF(target.translated(-pos()));
    const qreal band = 6.0 * scale;
    const qreal gap = 4.0 * scale;
    const qreal inset = 2.0 * scale;
    const QRectF inner = item.adjusted(-gap, -gap, gap, gap);
    const QRectF outer = inner.adjusted(-band, -band, band, band);

    QPainterPath outer_path;
    outer_path.addRoundedRect(outer, radius + gap + band * 0.8, radius + gap + band * 0.8);
    QPainterPath inner_path;
    inner_path.addRoundedRect(inner, radius + gap, radius + gap);
    QPainterPath item_path;
    item_path.addRoundedRect(item.adjusted(inset, inset, -inset, -inset), radius, radius);

    QConicalGradient gradient(outer.center(), angle);
    gradient.setColorAt(0.00, QColor(0x2a, 0x6b, 0xff));
    gradient.setColorAt(0.18, QColor(0x7b, 0x7c, 0xff));
    gradient.setColorAt(0.36, QColor(0xc6, 0x90, 0xff));
    gradient.setColorAt(0.52, QColor(0xff, 0x9f, 0xd0));
    gradient.setColorAt(0.68, QColor(0xbf, 0xf2, 0xff));
    gradient.setColorAt(0.84, QColor(0x6f, 0xb2, 0xff));
    gradient.setColorAt(1.00, QColor(0x2a, 0x6b, 0xff));
    painter.fillPath(outer_path.subtracted(inner_path), gradient);
    painter.fillPath(inner_path.subtracted(item_path), Qt::white);
}

OverlayDialog::OverlayDialog(QWidget* parent, Core::System& system, const QString& title_text,
                             const QString& body_text, const QString& left_button_text,
                             const QString& right_button_text, Qt::Alignment alignment,
                             bool use_rich_text_)
    : QDialog(parent), ui{std::make_unique<Ui::OverlayDialog>()}, use_rich_text{use_rich_text_} {
    ui->setupUi(this);
    setStyleSheet(OverlayStyleSheet(UseDarkOverlayTheme() ? DARK_PALETTE : LIGHT_PALETTE));

    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowTitleHint |
                   Qt::WindowSystemMenuHint | Qt::CustomizeWindowHint);
    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_TranslucentBackground);

    if (use_rich_text) {
        InitializeRichTextDialog(title_text, body_text, left_button_text, right_button_text,
                                 alignment);
    } else {
        InitializeRegularTextDialog(title_text, body_text, left_button_text, right_button_text,
                                    alignment);
    }

    MoveAndResizeWindow();

    focus_ring = new OverlayFocusRing(this);
    focus_ring->hide();
    for (QPushButton* button :
         {ui->button_cancel, ui->button_ok_label, ui->button_cancel_rich, ui->button_ok_rich}) {
        button->installEventFilter(this);
    }

    // TODO (Morph): Remove this when InputInterpreter no longer relies on the HID backend
    if (system.IsPoweredOn() && !ui->buttonsDialog->isHidden()) {
        input_interpreter = std::make_unique<InputInterpreter>(system);

        StartInputThread();
    }
}

OverlayDialog::~OverlayDialog() {
    StopInputThread();
}

void OverlayDialog::InitializeRegularTextDialog(const QString& title_text, const QString& body_text,
                                                const QString& left_button_text,
                                                const QString& right_button_text,
                                                Qt::Alignment alignment) {
    ui->stackedDialog->setCurrentIndex(0);

    ui->label_title->setText(title_text);
    ui->label_dialog->setText(body_text);
    ui->button_cancel->setText(left_button_text);
    ui->button_ok_label->setText(right_button_text);

    ui->label_dialog->setAlignment(alignment);

    if (title_text.isEmpty()) {
        ui->label_title->hide();
        ui->verticalLayout_2->setStretch(0, 0);
        ui->verticalLayout_2->setStretch(1, 219);
        ui->verticalLayout_2->setStretch(2, 82);
    }

    if (left_button_text.isEmpty()) {
        ui->button_cancel->hide();
        ui->button_cancel->setEnabled(false);
    }

    if (right_button_text.isEmpty()) {
        ui->button_ok_label->hide();
        ui->button_ok_label->setEnabled(false);
    }

    if (ui->button_cancel->isHidden() && ui->button_ok_label->isHidden()) {
        ui->buttonsDialog->hide();
        return;
    }

    connect(
        ui->button_cancel, &QPushButton::clicked, this,
        [this](bool) {
            StopInputThread();
            QDialog::reject();
        },
        Qt::QueuedConnection);
    connect(
        ui->button_ok_label, &QPushButton::clicked, this,
        [this](bool) {
            StopInputThread();
            QDialog::accept();
        },
        Qt::QueuedConnection);
}

void OverlayDialog::InitializeRichTextDialog(const QString& title_text, const QString& body_text,
                                             const QString& left_button_text,
                                             const QString& right_button_text,
                                             Qt::Alignment alignment) {
    ui->stackedDialog->setCurrentIndex(1);

    ui->label_title_rich->setText(title_text);
    ui->text_browser_dialog->setText(body_text);
    ui->button_cancel_rich->setText(left_button_text);
    ui->button_ok_rich->setText(right_button_text);

    // TODO (Morph/Rei): Replace this with something that works better
    ui->text_browser_dialog->setAlignment(alignment);

    if (title_text.isEmpty()) {
        ui->label_title_rich->hide();
        ui->verticalLayout_3->setStretch(0, 0);
        ui->verticalLayout_3->setStretch(1, 438);
        ui->verticalLayout_3->setStretch(2, 82);
    }

    if (left_button_text.isEmpty()) {
        ui->button_cancel_rich->hide();
        ui->button_cancel_rich->setEnabled(false);
    }

    if (right_button_text.isEmpty()) {
        ui->button_ok_rich->hide();
        ui->button_ok_rich->setEnabled(false);
    }

    if (ui->button_cancel_rich->isHidden() && ui->button_ok_rich->isHidden()) {
        ui->buttonsRichDialog->hide();
        return;
    }

    connect(
        ui->button_cancel_rich, &QPushButton::clicked, this,
        [this](bool) {
            StopInputThread();
            QDialog::reject();
        },
        Qt::QueuedConnection);
    connect(
        ui->button_ok_rich, &QPushButton::clicked, this,
        [this](bool) {
            StopInputThread();
            QDialog::accept();
        },
        Qt::QueuedConnection);
}

void OverlayDialog::MoveAndResizeWindow() {
    const auto pos = parentWidget()->mapToGlobal(parentWidget()->rect().topLeft());
    const auto width = static_cast<float>(parentWidget()->width());
    const auto height = static_cast<float>(parentWidget()->height());

    // High DPI
    const float dpi_scale = screen()->logicalDotsPerInch() / 96.0f;

    const auto title_text_font_size = BASE_TITLE_FONT_SIZE * (height / BASE_HEIGHT) / dpi_scale;
    const auto body_text_font_size =
        BASE_FONT_SIZE * (((width / BASE_WIDTH) + (height / BASE_HEIGHT)) / 2.0f) / dpi_scale;
    const auto button_text_font_size = BASE_FONT_SIZE * (height / BASE_HEIGHT) / dpi_scale;

    QFont title_text_font(QStringLiteral("MS Shell Dlg 2"), title_text_font_size, QFont::Normal);
    QFont body_text_font(QStringLiteral("MS Shell Dlg 2"), body_text_font_size, QFont::Normal);
    QFont button_text_font(QStringLiteral("MS Shell Dlg 2"), button_text_font_size, QFont::Normal);

    if (use_rich_text) {
        ui->label_title_rich->setFont(title_text_font);
        ui->text_browser_dialog->setFont(body_text_font);
        ui->button_cancel_rich->setFont(button_text_font);
        ui->button_ok_rich->setFont(button_text_font);
    } else {
        ui->label_title->setFont(title_text_font);
        ui->label_dialog->setFont(body_text_font);
        ui->button_cancel->setFont(button_text_font);
        ui->button_ok_label->setFont(button_text_font);
    }

    QDialog::move(pos);
    QDialog::resize(width, height);
}

template <Core::HID::NpadButton... T>
void OverlayDialog::HandleButtonPressedOnce() {
    const auto f = [this](Core::HID::NpadButton button) {
        if (input_interpreter->IsButtonPressedOnce(button)) {
            // Widgets belong to the GUI thread.
            QMetaObject::invokeMethod(
                this, [this, button] { TranslateButtonPress(button); }, Qt::QueuedConnection);
        }
    };

    (f(T), ...);
}

void OverlayDialog::TranslateButtonPress(Core::HID::NpadButton button) {
    QPushButton* left_button = use_rich_text ? ui->button_cancel_rich : ui->button_cancel;
    QPushButton* right_button = use_rich_text ? ui->button_ok_rich : ui->button_ok_label;

    // TODO (Morph): Handle QTextBrowser text scrolling
    // TODO (Morph): focusPrevious/NextChild() doesn't work well with the rich text dialog, fix it

    switch (button) {
    case Core::HID::NpadButton::A:
    case Core::HID::NpadButton::B:
        if (left_button->hasFocus()) {
            left_button->click();
        } else if (right_button->hasFocus()) {
            right_button->click();
        }
        break;
    case Core::HID::NpadButton::Left:
    case Core::HID::NpadButton::StickLLeft:
        focusPreviousChild();
        break;
    case Core::HID::NpadButton::Right:
    case Core::HID::NpadButton::StickLRight:
        focusNextChild();
        break;
    default:
        break;
    }
}

void OverlayDialog::StartInputThread() {
    input_thread = std::jthread([&](std::stop_token stoken) {
        while (!stoken.stop_requested()) {
            input_interpreter->PollInput();
            HandleButtonPressedOnce<Core::HID::NpadButton::A, Core::HID::NpadButton::B,
                                    Core::HID::NpadButton::Left, Core::HID::NpadButton::Right,
                                    Core::HID::NpadButton::StickLLeft,
                                    Core::HID::NpadButton::StickLRight>();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
}

void OverlayDialog::StopInputThread() {
    input_thread.request_stop();
}

bool OverlayDialog::eventFilter(QObject* object, QEvent* event) {
    auto* const button = qobject_cast<QPushButton*>(object);
    if (button == nullptr || focus_ring == nullptr) {
        return QDialog::eventFilter(object, event);
    }
    switch (event->type()) {
    case QEvent::FocusIn:
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::Show:
        if (button->hasFocus()) {
            focus_ring->Track(QRect(button->mapTo(this, QPoint(0, 0)), button->size()),
                              static_cast<float>(height()) / 1080.0f, BUTTON_RADIUS);
            focus_ring->show();
            focus_ring->raise();
        }
        break;
    case QEvent::FocusOut:
        focus_ring->hide();
        break;
    default:
        break;
    }
    return QDialog::eventFilter(object, event);
}

void OverlayDialog::keyPressEvent(QKeyEvent* e) {
    if (!ui->buttonsDialog->isHidden() || e->key() != Qt::Key_Escape) {
        QDialog::keyPressEvent(e);
    }
}
