// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <functional>
#include <string_view>

#include <QAbstractButton>
#include <QApplication>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QTextEdit>
#include <QTimer>

#include "common/logging.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/frontend/input_interpreter.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"
#include "yuzu/applets/qt_software_keyboard.h"
#include "yuzu/main_window.h"
#include "yuzu/util/overlay_dialog.h"

namespace QtSoftwareKeyboardWidgets {

// Positions and sizes are in pixels of a 1920x1080 game screen and scale with the dialog.
constexpr float REFERENCE_WIDTH = 1920.0f;
constexpr float REFERENCE_HEIGHT = 1080.0f;
constexpr float PANEL_TOP = 537.0f;
constexpr float LEGEND_CENTER = 1020.0f;

struct Theme {
    bool dark{};
    float scale{1.0f};
    QFont font;
    // The mouse moved over the keys since the last controller input: the key under it is tinted.
    bool hover_enabled{};

    QColor dim{0, 0, 0, 178};
    QColor panel;
    QColor key;
    QColor key_text;
    QColor key_hover;
    QColor key_pressed;
    QColor key_disabled;
    QColor key_disabled_text;
    QColor accent;
    QColor accent_text;
    QColor accent_pressed;
    QColor accent_disabled;
    QColor accent_disabled_text;
    QColor badge;
    QColor badge_text;
    QColor accent_badge;
    QColor accent_badge_text;
    QColor divider;
    QColor selected_tab;
    QColor shift_dot;
    QColor legend;
    QColor legend_disabled;
    QColor legend_glyph_text;

    // The text area is over the dimmed game screen, so it is the same in both themes.
    QColor field_text{0xff, 0xff, 0xff};
    QColor field_placeholder{0x9c, 0x9c, 0x9c};
    QColor field_line{0xf0, 0xf0, 0xf0};
    QColor field_count{0xe0, 0xe0, 0xe0};
    QColor field_caret{0x47, 0xaf, 0xfd};
    QColor header_text{0xf0, 0xf0, 0xf0};
    QColor sub_text{0xcc, 0xcc, 0xcc};

    float Px(float reference) const {
        return reference * scale;
    }

    QFont Font(float reference_size, QFont::Weight weight = QFont::Normal) const {
        QFont result = font;
        result.setPixelSize(std::max(1, static_cast<int>(std::lround(reference_size * scale))));
        result.setWeight(weight);
        return result;
    }
};

namespace {

void SetUpTheme(Theme& theme) {
    theme.dark = UseDarkOverlayTheme();
    theme.font = QApplication::font();
    if (theme.dark) {
        theme.panel = QColor(0x2e, 0x2e, 0x2e);
        theme.key = QColor(0x45, 0x45, 0x45);
        theme.key_text = QColor(0xee, 0xee, 0xee);
        theme.key_hover = QColor(0x3c, 0x51, 0x64);
        theme.key_pressed = QColor(0x5a, 0x5a, 0x5a);
        theme.key_disabled = QColor(0x27, 0x27, 0x27);
        theme.key_disabled_text = QColor(0x6b, 0x6b, 0x6b);
        theme.accent = QColor(0x3c, 0xb0, 0xe4);
        theme.accent_text = QColor(0x0c, 0x1a, 0x28);
        theme.accent_pressed = QColor(0x2f, 0x93, 0xc2);
        theme.accent_disabled = QColor(0x2c, 0x45, 0x52);
        theme.accent_disabled_text = QColor(0x5f, 0x76, 0x82);
        theme.badge = QColor(0xee, 0xee, 0xee);
        theme.badge_text = QColor(0x2e, 0x2e, 0x2e);
        theme.accent_badge = QColor(0x0c, 0x1a, 0x28);
        theme.accent_badge_text = QColor(0x3c, 0xb0, 0xe4);
        theme.divider = QColor(0x5e, 0x5e, 0x5e);
        theme.selected_tab = QColor(0x3c, 0xb0, 0xe4);
        theme.shift_dot = QColor(0x6b, 0x6b, 0x6b);
        theme.legend = QColor(0xee, 0xee, 0xee);
        theme.legend_disabled = QColor(0x6b, 0x6b, 0x6b);
        theme.legend_glyph_text = QColor(0x2e, 0x2e, 0x2e);
    } else {
        theme.panel = QColor(0xee, 0xee, 0xee);
        theme.key = QColor(0xfd, 0xfd, 0xfd);
        theme.key_text = QColor(0x10, 0x10, 0x10);
        theme.key_hover = QColor(0xe2, 0xed, 0xfa);
        theme.key_pressed = QColor(0xd2, 0xd2, 0xd2);
        theme.key_disabled = QColor(0xde, 0xde, 0xde);
        theme.key_disabled_text = QColor(0xbf, 0xbf, 0xbf);
        theme.accent = QColor(0x00, 0x65, 0xd6);
        theme.accent_text = QColor(0xff, 0xff, 0xff);
        theme.accent_pressed = QColor(0x00, 0x52, 0xae);
        theme.accent_disabled = QColor(0xbf, 0xc6, 0xce);
        theme.accent_disabled_text = QColor(0xe9, 0xed, 0xf2);
        theme.badge = QColor(0x10, 0x10, 0x10);
        theme.badge_text = QColor(0xff, 0xff, 0xff);
        theme.accent_badge = QColor(0xff, 0xff, 0xff);
        theme.accent_badge_text = QColor(0x00, 0x65, 0xd6);
        theme.divider = QColor(0xc8, 0xc8, 0xc8);
        theme.selected_tab = QColor(0x00, 0x65, 0xd6);
        theme.shift_dot = QColor(0xc4, 0xc4, 0xc4);
        theme.legend = QColor(0x10, 0x10, 0x10);
        theme.legend_disabled = QColor(0xbf, 0xbf, 0xbf);
        theme.legend_glyph_text = QColor(0xff, 0xff, 0xff);
    }
}

QPen RoundPen(const QColor& color, qreal width) {
    QPen pen(color, width);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    return pen;
}

/// A button symbol: a filled circle (or rounded square, for L and R) with a letter or a plus.
void DrawBadge(QPainter& painter, const QRectF& rect, QChar letter, const QColor& fill,
               const QColor& glyph, const QFont& base_font, bool square = false) {
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    if (square) {
        painter.drawRoundedRect(rect, rect.height() * 0.2, rect.height() * 0.2);
    } else {
        painter.drawEllipse(rect);
    }
    if (letter == QLatin1Char('+')) {
        const qreal arm = rect.width() * 0.27;
        QPen pen(glyph, std::max<qreal>(1.0, rect.width() * 0.14));
        pen.setCapStyle(Qt::FlatCap);
        painter.setPen(pen);
        const QPointF center = rect.center();
        painter.drawLine(QPointF(center.x() - arm, center.y()),
                         QPointF(center.x() + arm, center.y()));
        painter.drawLine(QPointF(center.x(), center.y() - arm),
                         QPointF(center.x(), center.y() + arm));
    } else {
        QFont font = base_font;
        font.setPixelSize(std::max(1, static_cast<int>(std::lround(rect.height() * 0.66))));
        font.setWeight(QFont::Bold);
        QPainterPath path;
        path.addText(0, 0, font, QString(letter));
        painter.translate(rect.center() - path.boundingRect().center());
        painter.fillPath(path, glyph);
    }
    painter.restore();
}

void DrawBackspaceIcon(QPainter& painter, const QPointF& center, float scale, const QColor& color) {
    const qreal w = 48 * scale;
    const qreal h = 32 * scale;
    const QRectF r(center.x() - w / 2, center.y() - h / 2, w, h);
    const qreal corner = 4 * scale;
    QPainterPath path;
    path.moveTo(r.left(), r.center().y());
    path.lineTo(r.left() + 13 * scale, r.top());
    path.lineTo(r.right() - corner, r.top());
    path.quadTo(r.right(), r.top(), r.right(), r.top() + corner);
    path.lineTo(r.right(), r.bottom() - corner);
    path.quadTo(r.right(), r.bottom(), r.right() - corner, r.bottom());
    path.lineTo(r.left() + 13 * scale, r.bottom());
    path.closeSubpath();
    painter.setPen(RoundPen(color, 3 * scale));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
    const QPointF cross(r.left() + 29 * scale, r.center().y());
    const qreal d = 6.5 * scale;
    painter.drawLine(cross + QPointF(-d, -d), cross + QPointF(d, d));
    painter.drawLine(cross + QPointF(-d, d), cross + QPointF(d, -d));
}

void DrawShiftIcon(QPainter& painter, const QPointF& center, float scale, const QColor& color,
                   bool filled) {
    const qreal w = 44 * scale;
    const qreal h = 40 * scale;
    const QRectF r(center.x() - w / 2, center.y() - h / 2, w, h);
    const qreal shoulder = r.top() + h * 0.52;
    const qreal stem = w * 0.23;
    QPainterPath path;
    path.moveTo(r.center().x(), r.top());
    path.lineTo(r.right(), shoulder);
    path.lineTo(r.center().x() + stem, shoulder);
    path.lineTo(r.center().x() + stem, r.bottom());
    path.lineTo(r.center().x() - stem, r.bottom());
    path.lineTo(r.center().x() - stem, shoulder);
    path.lineTo(r.left(), shoulder);
    path.closeSubpath();
    painter.setPen(RoundPen(color, 3 * scale));
    painter.setBrush(filled ? QBrush(color) : QBrush(Qt::NoBrush));
    painter.drawPath(path);
}

void DrawGlobeIcon(QPainter& painter, const QPointF& center, float scale, const QColor& color) {
    const qreal r = 21 * scale;
    painter.setPen(RoundPen(color, 2.6 * scale));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(center, r, r);
    painter.drawEllipse(center, r * 0.45, r);
    painter.drawLine(QPointF(center.x() - r, center.y()), QPointF(center.x() + r, center.y()));
    const qreal y = r * 0.5;
    const qreal x = std::sqrt(r * r - y * y);
    painter.drawLine(QPointF(center.x() - x, center.y() - y),
                     QPointF(center.x() + x, center.y() - y));
    painter.drawLine(QPointF(center.x() - x, center.y() + y),
                     QPointF(center.x() + x, center.y() + y));
}

void DrawArrow(QPainter& painter, const QRectF& rect, bool left, const QColor& color, float scale) {
    painter.setPen(RoundPen(color, 2.6 * scale));
    const qreal y = rect.center().y();
    const qreal head = rect.height() * 0.5;
    painter.drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
    const QPointF tip = left ? QPointF(rect.left(), y) : QPointF(rect.right(), y);
    const qreal back = left ? head : -head;
    painter.drawLine(tip, QPointF(tip.x() + back, y - head));
    painter.drawLine(tip, QPointF(tip.x() + back, y + head));
}

/// A control stick being pressed in: a chevron over the stick's cap, neck and base.
void DrawStickPressIcon(QPainter& painter, const QRectF& rect, const QColor& color, float scale) {
    const qreal cx = rect.center().x();
    painter.setPen(RoundPen(color, 2.6 * scale));
    painter.setBrush(Qt::NoBrush);
    painter.drawLine(QPointF(cx - rect.width() * 0.24, rect.top()),
                     QPointF(cx, rect.top() + rect.height() * 0.14));
    painter.drawLine(QPointF(cx, rect.top() + rect.height() * 0.14),
                     QPointF(cx + rect.width() * 0.24, rect.top()));
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QPointF(cx, rect.top() + rect.height() * 0.46), rect.width() * 0.42,
                        rect.height() * 0.2);
    painter.drawRect(QRectF(cx - rect.width() * 0.11, rect.top() + rect.height() * 0.5,
                            rect.width() * 0.22, rect.height() * 0.34));
    painter.drawEllipse(QPointF(cx, rect.bottom() - rect.height() * 0.07), rect.width() * 0.5,
                        rect.height() * 0.08);
}

/// A rectangle with only the left and/or right corners rounded (the ends of the tab row).
QPainterPath PartlyRoundedRect(const QRectF& rect, qreal radius, bool round_left,
                               bool round_right) {
    QPainterPath path;
    path.addRoundedRect(rect, radius, radius);
    if (!round_left) {
        QPainterPath square;
        square.addRect(QRectF(rect.left(), rect.top(), rect.width() / 2, rect.height()));
        path = path.united(square);
    }
    if (!round_right) {
        QPainterPath square;
        square.addRect(QRectF(rect.center().x(), rect.top(), rect.width() / 2, rect.height()));
        path = path.united(square);
    }
    return path.simplified();
}

} // Anonymous namespace

enum class KeyRole {
    Character,
    Backspace,
    Return,
    Ok,
    Globe,
    Shift,
    Tab,
    Space,
};

enum class ShiftState {
    Off,
    Once,
    Locked,
};

/// One key. Keys never take focus: OverlayFocusRing shows the controller's selection.
class Key final : public QAbstractButton {
public:
    explicit Key(QWidget* parent, const Theme* theme_, KeyRole role_)
        : QAbstractButton(parent), theme{theme_}, role{role_} {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
    }

    KeyRole Role() const {
        return role;
    }

    /// The text drawn on the key, and what a character key types.
    void SetLabel(const QString& label_) {
        label = label_;
        setText(label);
        setAccessibleName(label);
        update();
    }

    const QString& Label() const {
        return label;
    }

    /// A button symbol in the top right corner (B, + and, after "Space", Y).
    void SetBadge(QChar badge_) {
        badge = badge_;
    }

    /// Tabs: whether this is the first or last of the row, and whether its page is shown.
    void SetTabPosition(bool first, bool last) {
        first_tab = first;
        last_tab = last;
    }

    void SetCurrent(bool current_) {
        current = current_;
        update();
    }

    void SetShiftState(ShiftState state) {
        shift_state = state;
        update();
    }

    /// Shows the key pressed for a moment, for presses from a controller or the keyboard.
    void Flash() {
        flashing = true;
        update();
        QTimer::singleShot(110, this, [this] {
            flashing = false;
            update();
        });
    }

    std::function<void(Key*)> on_mouse_move;

protected:
    void paintEvent(QPaintEvent*) override;

    void mouseMoveEvent(QMouseEvent* event) override {
        QAbstractButton::mouseMoveEvent(event);
        if (on_mouse_move) {
            on_mouse_move(this);
        }
    }

    void leaveEvent(QEvent* event) override {
        QAbstractButton::leaveEvent(event);
        update();
    }

private:
    QFont FittedFont(const QFont& font, qreal room) const;
    void DrawCenteredText(QPainter& painter, const QRectF& rect, const QFont& font,
                          const QColor& color);

    const Theme* theme;
    KeyRole role;
    QString label;
    QChar badge;
    ShiftState shift_state{ShiftState::Off};
    bool first_tab{};
    bool last_tab{};
    bool current{};
    bool flashing{};
};

QFont Key::FittedFont(const QFont& font, qreal room) const {
    // Long labels (a game's own OK text, translations) shrink to fit.
    QFont fitted = font;
    const qreal width = QFontMetricsF(font).horizontalAdvance(label);
    if (width > room && room > 0 && fitted.pixelSize() > 0) {
        fitted.setPixelSize(std::max(1, static_cast<int>(fitted.pixelSize() * room / width)));
    }
    return fitted;
}

void Key::DrawCenteredText(QPainter& painter, const QRectF& rect, const QFont& font,
                           const QColor& color) {
    painter.setFont(FittedFont(font, rect.width() - theme->Px(16.0f)));
    painter.setPen(color);
    painter.drawText(rect, Qt::AlignCenter, label);
}

void Key::paintEvent(QPaintEvent*) {
    const Theme& t = *theme;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    const QRectF area = rect();
    const bool enabled = isEnabled();
    const bool pressed = enabled && (isDown() || flashing);
    const bool hovered = enabled && t.hover_enabled && underMouse();

    QColor face;
    QColor ink;
    if (role == KeyRole::Ok) {
        face = !enabled ? t.accent_disabled : pressed ? t.accent_pressed : t.accent;
        ink = enabled ? t.accent_text : t.accent_disabled_text;
    } else {
        face = !enabled ? t.key_disabled : pressed ? t.key_pressed : hovered ? t.key_hover : t.key;
        ink = enabled ? t.key_text : t.key_disabled_text;
    }

    const qreal radius = t.Px(4.0f);
    if (role == KeyRole::Tab) {
        painter.fillPath(PartlyRoundedRect(area, radius, first_tab, last_tab), face);
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(face);
        painter.drawRoundedRect(area, radius, radius);
    }

    switch (role) {
    case KeyRole::Character:
        DrawCenteredText(painter, area, t.Font(40.0f), ink);
        break;
    case KeyRole::Backspace:
        DrawBackspaceIcon(painter, area.center() - QPointF(t.Px(4.0f), 0), t.scale, ink);
        break;
    case KeyRole::Return:
        DrawCenteredText(painter, area, t.Font(34.0f), ink);
        break;
    case KeyRole::Ok:
        DrawCenteredText(painter, area, t.Font(38.0f), ink);
        break;
    case KeyRole::Globe:
        DrawGlobeIcon(painter, area.center(), t.scale, ink);
        break;
    case KeyRole::Shift: {
        DrawShiftIcon(painter, area.center(), t.scale, ink, shift_state != ShiftState::Off);
        // The Caps Lock light.
        const qreal dot = t.Px(9.0f);
        painter.setPen(Qt::NoPen);
        painter.setBrush(shift_state == ShiftState::Locked && enabled ? t.selected_tab
                                                                      : t.shift_dot);
        painter.drawEllipse(QRectF(t.Px(12.0f), t.Px(12.0f), dot, dot));
        break;
    }
    case KeyRole::Tab: {
        const QFont font = t.Font(31.0f);
        const QColor color = !enabled ? t.key_disabled_text : current ? t.selected_tab : ink;
        DrawCenteredText(painter, area, font, color);
        if (current && enabled) {
            const QFontMetricsF metrics(font);
            const qreal text_width = metrics.horizontalAdvance(label);
            const qreal baseline = area.center().y() + (metrics.ascent() - metrics.descent()) / 2.0;
            painter.fillRect(QRectF(area.center().x() - text_width / 2, baseline + t.Px(7.0f),
                                    text_width, t.Px(3.0f)),
                             color);
        }
        if (!first_tab) {
            painter.fillRect(QRectF(0, area.height() * 0.22, t.Px(2.0f), area.height() * 0.56),
                             t.divider);
        }
        break;
    }
    case KeyRole::Space:
        // The word stays centered; the Y symbol goes at the end of the bar, like B and +.
        DrawCenteredText(painter, area.adjusted(t.Px(50.0f), 0, -t.Px(50.0f), 0),
                         t.Font(34.0f), ink);
        break;
    }

    if (!badge.isNull()) {
        const qreal size = t.Px(27.0f);
        const QRectF corner(area.right() - t.Px(11.0f) - size, t.Px(10.0f), size, size);
        if (role == KeyRole::Ok) {
            DrawBadge(painter, corner, badge, enabled ? t.accent_badge : t.accent_disabled_text,
                      face, t.font);
        } else {
            DrawBadge(painter, corner, badge, enabled ? t.badge : t.key_disabled_text, face,
                      t.font);
        }
    }
}

/// The keyboard's background at the bottom of the screen, with the button guide.
class Panel final : public QWidget {
public:
    explicit Panel(QWidget* parent, const Theme* theme_) : QWidget(parent), theme{theme_} {
        setObjectName(QStringLiteral("oskPanel"));
    }

    void SetOrigin(float x_offset_) {
        x_offset = x_offset_;
    }

    void SetControllerPixmap(QPixmap pixmap) {
        controller = std::move(pixmap);
        update();
    }

    /// shift_text_ empty: no Shift entry (the number pad).
    void SetLegend(QString shift_text_, QString cancel_text_, QString enter_text_, bool left,
                   bool right) {
        shift_text = std::move(shift_text_);
        cancel_text = std::move(cancel_text_);
        enter_text = std::move(enter_text_);
        left_enabled = left;
        right_enabled = right;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    const Theme* theme;
    float x_offset{};
    QPixmap controller;
    QString shift_text;
    QString cancel_text;
    QString enter_text;
    bool left_enabled{};
    bool right_enabled{};
};

void Panel::paintEvent(QPaintEvent*) {
    const Theme& t = *theme;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    // Rounded top corners; the bottom ones fall outside the widget.
    const qreal radius = t.Px(24.0f);
    painter.setPen(Qt::NoPen);
    painter.setBrush(t.panel);
    painter.drawRoundedRect(QRectF(0, 0, width(), height() + radius), radius, radius);

    const qreal center_y = t.Px(LEGEND_CENTER - PANEL_TOP);
    if (!controller.isNull()) {
        const qreal h = t.Px(64.0f);
        const qreal w = h * controller.width() / std::max(1, controller.height());
        painter.drawPixmap(QRectF(x_offset + t.Px(101.0f), center_y - h / 2, w, h), controller,
                           controller.rect());
    }

    // The guide, right to left: Enter, Cancel, Shift, then the text cursor's R and L.
    const QFont font = t.Font(32.0f);
    const QFontMetricsF metrics(font);
    painter.setFont(font);
    qreal right = x_offset + t.Px(1818.0f);
    const auto text = [&](const QString& string, const QColor& color) {
        const qreal w = metrics.horizontalAdvance(string);
        painter.setPen(color);
        painter.drawText(QRectF(right - w - 1, center_y - t.Px(30.0f), w + 2, t.Px(60.0f)),
                         Qt::AlignCenter, string);
        right -= w;
    };
    const auto gap = [&](float reference) { right -= t.Px(reference); };
    const auto round_symbol = [&](QChar letter, const QColor& color) {
        const qreal d = t.Px(34.0f);
        DrawBadge(painter, QRectF(right - d, center_y - d / 2, d, d), letter, color,
                  t.legend_glyph_text, t.font);
        right -= d;
    };
    const auto square_symbol = [&](QChar letter, const QColor& color) {
        const qreal w = t.Px(31.0f);
        const qreal h = t.Px(28.0f);
        DrawBadge(painter, QRectF(right - w, center_y - h / 2, w, h), letter, color,
                  t.legend_glyph_text, t.font, true);
        right -= w;
    };
    const auto arrow = [&](bool left, const QColor& color) {
        const qreal w = t.Px(29.0f);
        DrawArrow(painter, QRectF(right - w, center_y - t.Px(10.0f), w, t.Px(20.0f)), left, color,
                  t.scale);
        right -= w;
    };

    text(enter_text, t.legend);
    gap(10.0f);
    round_symbol(QLatin1Char('A'), t.legend);
    gap(46.0f);
    text(cancel_text, t.legend);
    gap(10.0f);
    round_symbol(QLatin1Char('X'), t.legend);
    gap(46.0f);
    if (!shift_text.isEmpty()) {
        text(shift_text, t.legend);
        gap(9.0f);
        const qreal w = t.Px(28.0f);
        DrawStickPressIcon(painter, QRectF(right - w, center_y - t.Px(18.0f), w, t.Px(36.0f)),
                           t.legend, t.scale);
        right -= w;
        gap(46.0f);
    }
    const QColor right_color = right_enabled ? t.legend : t.legend_disabled;
    arrow(false, right_color);
    gap(10.0f);
    square_symbol(QLatin1Char('R'), right_color);
    gap(46.0f);
    const QColor left_color = left_enabled ? t.legend : t.legend_disabled;
    arrow(true, left_color);
    gap(10.0f);
    square_symbol(QLatin1Char('L'), left_color);
}

/// The single-line text field. Draws a wide blue caret over Qt's thin one.
class LineEdit final : public QLineEdit {
public:
    explicit LineEdit(QWidget* parent, const Theme* theme_) : QLineEdit(parent), theme{theme_} {
        setObjectName(QStringLiteral("oskLineEdit"));
        setFrame(false);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QLineEdit::paintEvent(event);
        if (!hasFocus() || hasSelectedText()) {
            return;
        }
        QPainter painter(this);
        const QRect cursor = cursorRect();
        const qreal width = std::max<qreal>(2.0, theme->Px(4.0f));
        painter.fillRect(QRectF(cursor.center().x() + 0.5 - width / 2, cursor.top() + 1, width,
                                cursor.height() - 2),
                         theme->field_caret);
    }

private:
    const Theme* theme;
};

/// The multi-line text field (Box mode), with the same caret.
class TextEdit final : public QTextEdit {
public:
    explicit TextEdit(QWidget* parent, const Theme* theme_) : QTextEdit(parent), theme{theme_} {
        setObjectName(QStringLiteral("oskTextEdit"));
        setFrameShape(QFrame::NoFrame);
        setAcceptRichText(false);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QTextEdit::paintEvent(event);
        if (!hasFocus() || textCursor().hasSelection()) {
            return;
        }
        QPainter painter(viewport());
        const QRect cursor = cursorRect();
        const qreal width = std::max<qreal>(2.0, theme->Px(4.0f));
        painter.fillRect(
            QRectF(cursor.left() + 0.5 - width / 2, cursor.top() + 1, width, cursor.height() - 2),
            theme->field_caret);
    }

private:
    const Theme* theme;
};

} // namespace QtSoftwareKeyboardWidgets

namespace {

using namespace Service::AM::Frontend;
using QtSoftwareKeyboardWidgets::KeyRole;
using QtSoftwareKeyboardWidgets::LineEdit;
using QtSoftwareKeyboardWidgets::Panel;
using QtSoftwareKeyboardWidgets::ShiftState;
using QtSoftwareKeyboardWidgets::TextEdit;
using QtSoftwareKeyboardWidgets::Theme;

using KeyRows = std::array<std::u16string_view, 4>;

// The letter rows of the Nintendo Switch 2's English keyboard, and its Shift rows.
constexpr KeyRows LETTERS_LOWER{u"1234567890-", u"qwertyuiop/", u"asdfghjkl:'", u"zxcvbnm,.?!"};
constexpr KeyRows LETTERS_UPPER{u"1234567890@", u"QWERTYUIOP=", u"ASDFGHJKL&;", u"ZXCVBNM*#!?"};
// Qwerty keyboards (passwords, web addresses).
constexpr KeyRows URL_LOWER{u"1234567890@", u"qwertyuiop+", u"asdfghjkl_:", u"zxcvbnm,.-/"};
constexpr KeyRows URL_UPPER{u"1234567890@", u"QWERTYUIOP+", u"ASDFGHJKL_:", u"ZXCVBNM,.-/"};
// #+=: every ASCII symbol, then more with Shift.
constexpr KeyRows SYMBOLS{u"!?@#$%&*()_", u"\"'`^~\\|/<>-", u"[]{}+=:;,.…", u"¿¡«»€£¥¢°•·"};
constexpr KeyRows SYMBOLS_SHIFT{u"×÷±≠≈≤≥∞√¬‰", u"‘’“”‚„‹›′″¦", u"©®™§¶†‡µ¹²³", u"¼½¾←→↑↓♪☆★♥"};
// ÀÁÂ: vowels with marks.
constexpr KeyRows VOWELS_LOWER{u"àáâäãåāăąæè", u"éêëēėęěìíîï", u"īįıòóôöõøōő", u"œùúûüūůűųýÿ"};
constexpr KeyRows VOWELS_UPPER{u"ÀÁÂÄÃÅĀĂĄÆÈ", u"ÉÊËĒĖĘĚÌÍÎÏ", u"ĪĮİÒÓÔÖÕØŌŐ", u"ŒÙÚÛÜŪŮŰŲÝŸ"};
// ÑŃŅ: consonants with marks.
constexpr KeyRows CONSONANTS_LOWER{u"ñńňņŋçćčĉċß", u"śšşșŝźžżłľĺ", u"ļŀřŕŗťţțŧďđ", u"ðğģĝġþķĥħĵŵ"};
constexpr KeyRows CONSONANTS_UPPER{u"ÑŃŇŅŊÇĆČĈĊẞ", u"ŚŠŞȘŜŹŽŻŁĽĹ", u"ĻĿŘŔŖŤŢȚŦĎĐ", u"ÐĞĢĜĠÞĶĤĦĴŴ"};

constexpr bool HasElevenKeys(const KeyRows& rows) {
    return std::all_of(rows.begin(), rows.end(),
                       [](std::u16string_view r) { return r.size() == 11; });
}
static_assert(HasElevenKeys(LETTERS_LOWER) && HasElevenKeys(LETTERS_UPPER) &&
              HasElevenKeys(URL_LOWER) && HasElevenKeys(URL_UPPER) && HasElevenKeys(SYMBOLS) &&
              HasElevenKeys(SYMBOLS_SHIFT) && HasElevenKeys(VOWELS_LOWER) &&
              HasElevenKeys(VOWELS_UPPER) && HasElevenKeys(CONSONANTS_LOWER) &&
              HasElevenKeys(CONSONANTS_UPPER));

} // Anonymous namespace

QtSoftwareKeyboardDialog::QtSoftwareKeyboardDialog(
    QWidget* parent, Core::System& system_, bool is_inline_,
    Core::Frontend::KeyboardInitializeParameters initialize_parameters_)
    : QDialog(parent), system{system_}, is_inline{is_inline_},
      initialize_parameters{std::move(initialize_parameters_)}, theme{std::make_unique<Theme>()} {
    setObjectName(QStringLiteral("QtSoftwareKeyboardDialog"));
    setWindowTitle(tr("Software Keyboard"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowTitleHint |
                   Qt::WindowSystemMenuHint | Qt::CustomizeWindowHint);
    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_TranslucentBackground);

    QtSoftwareKeyboardWidgets::SetUpTheme(*theme);

    // The app's theme styles every widget; the keyboard draws its own.
    setStyleSheet(QStringLiteral(
                      "QDialog#QtSoftwareKeyboardDialog, QWidget#oskPanel,"
                      " QWidget#oskPanel QAbstractButton { background: transparent; border: none; }"
                      "QLineEdit#oskLineEdit, QTextEdit#oskTextEdit { background: transparent;"
                      " border: none; padding: 0px; color: %1; selection-color: #ffffff;"
                      " selection-background-color: %2; }")
                      .arg(theme->field_text.name(), QColor(0x00, 0x65, 0xd6).name()));

    line_edit = new LineEdit(this, theme.get());
    text_edit = new TextEdit(this, theme.get());
    for (QWidget* field : {static_cast<QWidget*>(line_edit), static_cast<QWidget*>(text_edit)}) {
        QPalette palette = field->palette();
        palette.setColor(QPalette::PlaceholderText, theme->field_placeholder);
        palette.setColor(QPalette::Text, theme->field_text);
        field->setPalette(palette);
    }

    panel = new Panel(this, theme.get());
    CreateKeys();
    focus_ring = new OverlayFocusRing(panel);
    focus_ring->raise();

    if (!initialize_parameters.ok_text.empty()) {
        key_ok->SetLabel(QString::fromStdU16String(initialize_parameters.ok_text));
        key_ok_num->SetLabel(key_ok->Label());
    }

    if (initialize_parameters.left_optional_symbol_key != u'\0') {
        key_left_optional->SetLabel(QChar{initialize_parameters.left_optional_symbol_key});
    }
    if (initialize_parameters.right_optional_symbol_key != u'\0') {
        key_right_optional->SetLabel(QChar{initialize_parameters.right_optional_symbol_key});
    }

    current_text = initialize_parameters.initial_text;
    cursor_position = initialize_parameters.initial_cursor_position;

    SetTextDrawType();

    for (Key* key : all_keys) {
        connect(key, &QAbstractButton::clicked, this, [this, key](bool) {
            if (is_inline) {
                InlineKeyboardButtonClicked(key);
            } else {
                NormalKeyboardButtonClicked(key);
            }
        });
        key->on_mouse_move = [this](Key* hovered) { KeyHovered(hovered); };
    }

    // TODO (Morph): Remove this when InputInterpreter no longer relies on the HID backend
    if (system.IsPoweredOn()) {
        input_interpreter = std::make_unique<InputInterpreter>(system);
    }
}

QtSoftwareKeyboardDialog::~QtSoftwareKeyboardDialog() {
    StopInputThread();
}

void QtSoftwareKeyboardDialog::CreateKeys() {
    const auto make = [this](KeyRole role, const QString& label = {}) {
        auto* key = new Key(panel, theme.get(), role);
        key->SetLabel(label);
        all_keys.push_back(key);
        return key;
    };

    for (auto& keys : character_keys) {
        for (auto& key : keys) {
            key = make(KeyRole::Character);
        }
    }
    key_backspace = make(KeyRole::Backspace, tr("Backspace"));
    key_backspace->SetBadge(QLatin1Char('B'));
    key_return = make(KeyRole::Return, tr("Return"));
    key_ok = make(KeyRole::Ok, tr("OK"));
    key_ok->SetBadge(QLatin1Char('+'));
    key_globe = make(KeyRole::Globe, tr("Keyboard language"));
    key_shift = make(KeyRole::Shift, tr("Shift"));
    key_space = make(KeyRole::Space, tr("Space"));
    key_space->SetBadge(QLatin1Char('Y'));
    const std::array<QString, 4> tab_labels{QStringLiteral("ABC"), QStringLiteral("#+="),
                                            QStringLiteral("ÀÁÂ"), QStringLiteral("ÑŃŅ")};
    for (std::size_t i = 0; i < tab_keys.size(); ++i) {
        tab_keys[i] = make(KeyRole::Tab, tab_labels[i]);
    }

    for (std::size_t i = 0; i < number_keys.size(); ++i) {
        number_keys[i] = make(KeyRole::Character, QString::number(i));
    }
    key_left_optional = make(KeyRole::Character);
    key_right_optional = make(KeyRole::Character);
    key_backspace_num = make(KeyRole::Backspace, tr("Backspace"));
    key_backspace_num->SetBadge(QLatin1Char('B'));
    key_ok_num = make(KeyRole::Ok, tr("OK"));
    key_ok_num->SetBadge(QLatin1Char('+'));

    numberpad_buttons = {{
        {number_keys[1], number_keys[2], number_keys[3], key_backspace_num},
        {number_keys[4], number_keys[5], number_keys[6], key_ok_num},
        {number_keys[7], number_keys[8], number_keys[9], key_ok_num},
        {key_left_optional, number_keys[0], key_right_optional, key_ok_num},
    }};
}

void QtSoftwareKeyboardDialog::ShowNormalKeyboard(QPoint pos, QSize size) {
    if (isVisible()) {
        return;
    }

    SetKeyboardType();
    SetPasswordMode();
    SetControllerImage();
    DisableKeyboardButtons();
    SetBackspaceOkEnabled();
    MoveAndResizeWindow(pos, size);

    open();
}

void QtSoftwareKeyboardDialog::ShowTextCheckDialog(
    Service::AM::Frontend::SwkbdTextCheckResult text_check_result,
    std::u16string text_check_message) {
    switch (text_check_result) {
    case SwkbdTextCheckResult::Success:
    case SwkbdTextCheckResult::Silent:
    default:
        break;
    case SwkbdTextCheckResult::Failure: {
        StopInputThread();

        OverlayDialog dialog(this, system, QString{}, QString::fromStdU16String(text_check_message),
                             QString{}, tr("OK"), Qt::AlignCenter);
        dialog.exec();

        StartInputThread();
        break;
    }
    case SwkbdTextCheckResult::Confirm: {
        StopInputThread();

        OverlayDialog dialog(this, system, QString{}, QString::fromStdU16String(text_check_message),
                             tr("Cancel"), tr("OK"), Qt::AlignCenter);
        if (dialog.exec() != QDialog::Accepted) {
            StartInputThread();
            break;
        }

        const auto text = CurrentText();
        auto text_str = Common::U16StringFromBuffer(text.utf16(), text.size());

        emit SubmitNormalText(SwkbdResult::Ok, std::move(text_str), true);
        break;
    }
    }
}

void QtSoftwareKeyboardDialog::ShowInlineKeyboard(
    Core::Frontend::InlineAppearParameters appear_parameters, QPoint pos, QSize size) {
    line_edit->hide();
    text_edit->hide();

    initialize_parameters.max_text_length = appear_parameters.max_text_length;
    initialize_parameters.min_text_length = appear_parameters.min_text_length;
    initialize_parameters.type = appear_parameters.type;
    initialize_parameters.key_disable_flags = appear_parameters.key_disable_flags;
    initialize_parameters.enable_backspace_button = appear_parameters.enable_backspace_button;
    initialize_parameters.enable_return_button = appear_parameters.enable_return_button;
    initialize_parameters.disable_cancel_button = appear_parameters.disable_cancel_button;

    SetKeyboardType();
    SetControllerImage();
    DisableKeyboardButtons();
    SetBackspaceOkEnabled();
    MoveAndResizeWindow(pos, size);

    open();
}

void QtSoftwareKeyboardDialog::HideInlineKeyboard() {
    StopInputThread();
    QDialog::hide();
}

void QtSoftwareKeyboardDialog::InlineTextChanged(
    Core::Frontend::InlineTextParameters text_parameters) {
    current_text = text_parameters.input_text;
    cursor_position = text_parameters.cursor_position;

    SetBackspaceOkEnabled();
    UpdateLegend();
}

void QtSoftwareKeyboardDialog::ExitKeyboard() {
    StopInputThread();
    QDialog::done(QDialog::Accepted);
}

void QtSoftwareKeyboardDialog::PressButtonForTest(Core::HID::NpadButton button) {
    TranslateButtonPress(button);
}

void QtSoftwareKeyboardDialog::open() {
    QDialog::open();

    // QDialog gives its first field focus as if by Tab, which selects all of a line's text.
    if (!is_inline && !box_mode) {
        line_edit->setCursorPosition(std::clamp(initialize_parameters.initial_cursor_position, 0,
                                                static_cast<int>(line_edit->text().size())));
    }

    row = 0;
    column = 0;
    theme->hover_enabled = false;
    focus_ring->show();
    UpdateFocusRing();
    UpdateLegend();

    StartInputThread();
}

void QtSoftwareKeyboardDialog::reject() {
    // Pressing the ESC key in a dialog calls QDialog::reject().
    // We will override this behavior to the "Cancel" action on the software keyboard.
    TranslateButtonPress(Core::HID::NpadButton::X);
}

void QtSoftwareKeyboardDialog::keyPressEvent(QKeyEvent* event) {
    if (!is_inline) {
        QDialog::keyPressEvent(event);
        return;
    }

    const auto entered_key = event->key();

    switch (entered_key) {
    case Qt::Key_Escape:
        QDialog::keyPressEvent(event);
        return;
    case Qt::Key_Backspace:
        (page == Page::NumberPad ? key_backspace_num : key_backspace)->click();
        return;
    case Qt::Key_Return:
        (page == Page::NumberPad ? key_ok_num : key_ok)->click();
        return;
    case Qt::Key_Left:
        MoveTextCursorDirection(Direction::Left);
        return;
    case Qt::Key_Right:
        MoveTextCursorDirection(Direction::Right);
        return;
    default:
        break;
    }

    const auto entered_text = event->text();

    if (entered_text.isEmpty()) {
        return;
    }

    InlineTextInsertString(Common::U16StringFromBuffer(entered_text.utf16(), entered_text.size()));
}

void QtSoftwareKeyboardDialog::paintEvent(QPaintEvent*) {
    const Theme& t = *theme;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    if (!is_inline) {
        painter.fillRect(rect(), t.dim);

        if (box_mode) {
            const qreal pen_width = t.Px(3.0f);
            painter.setPen(QPen(t.field_line, pen_width));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(
                QRectF(text_field_rect)
                    .adjusted(pen_width / 2, pen_width / 2, -pen_width / 2, -pen_width / 2),
                t.Px(12.0f), t.Px(12.0f));
        } else {
            painter.fillRect(text_field_rect, t.field_line);
            painter.setFont(t.Font(40.0f));
            painter.setPen(t.header_text);
            painter.drawText(header_rect, Qt::AlignLeft | Qt::AlignVCenter,
                             QString::fromStdU16String(initialize_parameters.header_text));
            painter.setFont(t.Font(30.0f));
            painter.setPen(t.sub_text);
            painter.drawText(sub_rect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                             QString::fromStdU16String(initialize_parameters.sub_text));
        }

        painter.setFont(t.Font(30.0f));
        painter.setPen(t.field_count);
        painter.drawText(character_count_rect, Qt::AlignRight | Qt::AlignTop, character_count);
    }

    // A soft shadow along the panel's top edge.
    const QRectF panel_rect(panel->geometry());
    const qreal radius = t.Px(24.0f);
    painter.setPen(Qt::NoPen);
    for (int i = 4; i >= 1; --i) {
        painter.setBrush(QColor(0, 0, 0, t.dark ? 34 : 14));
        painter.drawRoundedRect(panel_rect.adjusted(0, -t.Px(2.5f) * i, 0, radius), radius, radius);
    }
}

void QtSoftwareKeyboardDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    LayoutKeyboard();
}

void QtSoftwareKeyboardDialog::MoveAndResizeWindow(QPoint pos, QSize size) {
    QDialog::move(pos);
    QDialog::resize(size);
    LayoutKeyboard();
}

void QtSoftwareKeyboardDialog::LayoutKeyboard() {
    using namespace QtSoftwareKeyboardWidgets;

    const float scale = std::min(width() / REFERENCE_WIDTH, height() / REFERENCE_HEIGHT);
    if (scale <= 0.0f) {
        return;
    }
    theme->scale = scale;
    const float x_offset = (width() - REFERENCE_WIDTH * scale) / 2.0f;
    const int panel_top =
        static_cast<int>(std::lround(height() - (REFERENCE_HEIGHT - PANEL_TOP) * scale));
    panel->setGeometry(0, panel_top, width(), height() - panel_top);
    panel->SetOrigin(x_offset);

    // Each edge is rounded on its own, so neighbouring keys keep even gaps.
    const auto edge_x = [&](float x) {
        return static_cast<int>(std::lround(x_offset + x * scale));
    };
    const auto edge_y = [&](float y) { return static_cast<int>(std::lround(y * scale)); };
    const auto ref_rect = [&](float x, float y, float w, float h) {
        return QRect(QPoint(edge_x(x), edge_y(y)), QPoint(edge_x(x + w) - 1, edge_y(y + h) - 1));
    };
    const auto place = [&](Key* key, float x, float y, float w, float h) {
        key->setGeometry(ref_rect(x, y - PANEL_TOP, w, h));
    };

    // Letters: eleven columns of 128 with gaps of 6, then the column of wide keys.
    for (std::size_t r = 0; r < NUM_CHARACTER_ROWS; ++r) {
        for (std::size_t c = 0; c < NUM_CHARACTER_COLUMNS; ++c) {
            place(character_keys[r][c], 133.0f + c * 134.0f, 569.0f + r * 76.0f, 128.0f, 70.0f);
        }
    }
    place(key_backspace, 1617.0f, 569.0f, 170.0f, 70.0f);
    place(key_return, 1617.0f, 645.0f, 170.0f, 146.0f);
    place(key_ok, 1617.0f, 797.0f, 170.0f, 156.0f);
    place(key_globe, 133.0f, 883.0f, 128.0f, 70.0f);
    place(key_shift, 267.0f, 883.0f, 128.0f, 70.0f);
    const std::size_t tab_count = tab_keys[2]->isHidden() ? 2 : 4;
    const float tabs_width = tab_count * 134.0f - 6.0f;
    for (std::size_t i = 0; i < tab_count; ++i) {
        place(tab_keys[i], 401.0f + i * tabs_width / tab_count, 883.0f, tabs_width / tab_count,
              70.0f);
        tab_keys[i]->SetTabPosition(i == 0, i == tab_count - 1);
    }
    const float space_left = 401.0f + tabs_width + 6.0f;
    place(key_space, space_left, 883.0f, 1601.0f - space_left, 70.0f);

    // The number pad: three columns of 246 with gaps of 6, centered, and Backspace and OK.
    for (std::size_t r = 0; r < NUM_ROWS_NUMPAD; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            place(numberpad_buttons[r][c], 585.0f + c * 252.0f, 569.0f + r * 97.0f, 246.0f, 91.0f);
        }
    }
    place(key_backspace_num, 1351.0f, 569.0f, 164.0f, 91.0f);
    place(key_ok_num, 1351.0f, 666.0f, 164.0f, 285.0f);

    // The text area over the game screen.
    if (box_mode) {
        text_field_rect = ref_rect(128.0f, 80.0f, 1664.0f, 283.0f);
        text_edit->setGeometry(
            text_field_rect.adjusted(edge_y(24.0f), edge_y(18.0f), -edge_y(24.0f), -edge_y(14.0f)));
        character_count_rect = ref_rect(1392.0f, 372.0f, 400.0f, 44.0f);
    } else {
        const float left = wide_line ? 128.0f : 585.0f;
        const float field_width = wide_line ? 1664.0f : 750.0f;
        text_field_rect = ref_rect(left, 366.0f, field_width, 4.0f);
        line_edit->setGeometry(ref_rect(left, 280.0f, field_width, 82.0f));
        character_count_rect = ref_rect(left + field_width - 400.0f, 378.0f, 400.0f, 44.0f);
        header_rect = ref_rect(128.0f, 62.0f, 1664.0f, 64.0f);
        sub_rect = ref_rect(128.0f, 132.0f, 1664.0f, 120.0f);
    }
    line_edit->setFont(theme->Font(46.0f));
    text_edit->setFont(theme->Font(44.0f));

    UpdateFocusRing();
    update();
}

void QtSoftwareKeyboardDialog::SetKeyboardType() {
    const bool download_code = initialize_parameters.key_disable_flags.download_code != 0;

    switch (initialize_parameters.type) {
    case SwkbdType::NumberPad:
        page = Page::NumberPad;
        url_layout = false;
        break;
    case SwkbdType::Qwerty:
        page = Page::Letters;
        url_layout = true;
        break;
    case SwkbdType::Normal:
    case SwkbdType::Unknown3:
    case SwkbdType::Latin:
    case SwkbdType::SimplifiedChinese:
    case SwkbdType::TraditionalChinese:
    case SwkbdType::Korean:
    default:
        page = Page::Letters;
        url_layout = false;
        break;
    }

    const bool numberpad = page == Page::NumberPad;
    for (const auto& keys : character_keys) {
        for (Key* key : keys) {
            key->setVisible(!numberpad);
        }
    }
    for (Key* key : {key_backspace, key_return, key_ok, key_globe, key_shift, key_space}) {
        key->setVisible(!numberpad);
    }
    // Qwerty keyboards and download codes have no pages of accented letters.
    for (std::size_t i = 0; i < tab_keys.size(); ++i) {
        tab_keys[i]->setVisible(!numberpad && (i < 2 || !(url_layout || download_code)));
    }
    for (Key* key : number_keys) {
        key->setVisible(numberpad);
    }
    for (Key* key : {key_backspace_num, key_ok_num}) {
        key->setVisible(numberpad);
    }
    key_left_optional->setVisible(numberpad &&
                                  initialize_parameters.left_optional_symbol_key != u'\0');
    key_right_optional->setVisible(numberpad &&
                                   initialize_parameters.right_optional_symbol_key != u'\0');

    // Download codes are typed in capitals.
    shift_enabled = download_code;
    caps_lock_enabled = download_code;

    // The navigation grid of the letter pages. Keys spanning cells repeat.
    for (std::size_t r = 0; r < NUM_CHARACTER_ROWS; ++r) {
        for (std::size_t c = 0; c < NUM_CHARACTER_COLUMNS; ++c) {
            keyboard_buttons[r][c] = character_keys[r][c];
        }
    }
    keyboard_buttons[0][11] = key_backspace;
    keyboard_buttons[1][11] = key_return;
    keyboard_buttons[2][11] = key_return;
    keyboard_buttons[3][11] = key_ok;
    auto& bottom_row = keyboard_buttons[4];
    bottom_row[0] = key_globe;
    bottom_row[1] = key_shift;
    std::size_t next_column = 2;
    for (Key* tab : tab_keys) {
        if (!tab->isHidden()) {
            bottom_row[next_column++] = tab;
        }
    }
    while (next_column < NUM_COLUMNS_NORMAL - 1) {
        bottom_row[next_column++] = key_space;
    }
    bottom_row[11] = key_ok;

    if (!numberpad) {
        SetPage(Page::Letters);
    }
    LayoutKeyboard();
}

void QtSoftwareKeyboardDialog::SetPasswordMode() {
    switch (initialize_parameters.password_mode) {
    case SwkbdPasswordMode::Disabled:
    default:
        line_edit->setEchoMode(QLineEdit::Normal);
        break;
    case SwkbdPasswordMode::Enabled:
        line_edit->setEchoMode(QLineEdit::Password);
        break;
    }
}

void QtSoftwareKeyboardDialog::SetTextDrawType() {
    switch (initialize_parameters.text_draw_type) {
    case SwkbdTextDrawType::Line:
    case SwkbdTextDrawType::DownloadCode: {
        box_mode = false;
        wide_line = initialize_parameters.max_text_length > 10;
        text_edit->hide();

        if (is_inline) {
            line_edit->hide();
            return;
        }

        connect(line_edit, &QLineEdit::textChanged, this, [this](const QString& changed_string) {
            const auto is_valid = ValidateInputText(changed_string);

            key_ok->setEnabled(is_valid);
            key_ok_num->setEnabled(is_valid);

            UpdateCharacterCount();
            UpdateLegend();
            line_edit->setFocus();
        });

        connect(line_edit, &QLineEdit::cursorPositionChanged, this,
                [this](int old_cursor_position, int new_cursor_position) {
                    const bool enable =
                        initialize_parameters.enable_backspace_button && new_cursor_position > 0;
                    key_backspace->setEnabled(enable);
                    key_backspace_num->setEnabled(enable);

                    UpdateLegend();
                    line_edit->setFocus();
                });

        connect(
            line_edit, &QLineEdit::returnPressed, this,
            [this] { TranslateButtonPress(Core::HID::NpadButton::Plus); }, Qt::QueuedConnection);

        line_edit->setPlaceholderText(QString::fromStdU16String(initialize_parameters.guide_text));
        line_edit->setText(QString::fromStdU16String(initialize_parameters.initial_text));
        line_edit->setMaxLength(initialize_parameters.max_text_length);
        line_edit->setCursorPosition(initialize_parameters.initial_cursor_position);
        break;
    }
    case SwkbdTextDrawType::Box:
    default: {
        box_mode = true;
        line_edit->hide();

        if (is_inline) {
            text_edit->hide();
            return;
        }

        connect(text_edit, &QTextEdit::textChanged, this, [this] {
            if (static_cast<u32>(text_edit->toPlainText().length()) >
                initialize_parameters.max_text_length) {
                auto text_cursor = text_edit->textCursor();
                text_edit->setTextCursor(text_cursor);
                text_cursor.deletePreviousChar();
            }

            const auto is_valid = ValidateInputText(text_edit->toPlainText());

            key_ok->setEnabled(is_valid);
            key_ok_num->setEnabled(is_valid);

            UpdateCharacterCount();
            UpdateLegend();
            text_edit->setFocus();
        });

        connect(text_edit, &QTextEdit::cursorPositionChanged, this, [this] {
            const auto new_cursor_position = text_edit->textCursor().position();
            const bool enable =
                initialize_parameters.enable_backspace_button && new_cursor_position > 0;
            key_backspace->setEnabled(enable);
            key_backspace_num->setEnabled(enable);

            UpdateLegend();
            text_edit->setFocus();
        });

        text_edit->setPlaceholderText(QString::fromStdU16String(initialize_parameters.guide_text));
        text_edit->setText(QString::fromStdU16String(initialize_parameters.initial_text));
        text_edit->moveCursor(initialize_parameters.initial_cursor_position == 0
                                  ? QTextCursor::Start
                                  : QTextCursor::End);
        break;
    }
    }

    UpdateCharacterCount();
}

void QtSoftwareKeyboardDialog::SetControllerImage() {
    const auto* handheld = system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Handheld);
    const auto* player_1 = system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    const auto controller_type =
        handheld->IsConnected() ? handheld->GetNpadStyleIndex() : player_1->GetNpadStyleIndex();

    const QString suffix = theme->dark ? QStringLiteral("_dark") : QString{};

    QString name;
    switch (controller_type) {
    case Core::HID::NpadStyleIndex::Fullkey:
    case Core::HID::NpadStyleIndex::GameCube:
        name = QStringLiteral("controller_pro");
        break;
    case Core::HID::NpadStyleIndex::JoyconDual:
        name = QStringLiteral("controller_dual_joycon");
        break;
    case Core::HID::NpadStyleIndex::JoyconLeft:
        name = QStringLiteral("controller_single_joycon_left");
        break;
    case Core::HID::NpadStyleIndex::JoyconRight:
        name = QStringLiteral("controller_single_joycon_right");
        break;
    case Core::HID::NpadStyleIndex::Handheld:
        name = QStringLiteral("controller_handheld");
        break;
    default:
        break;
    }

    panel->SetControllerPixmap(
        name.isEmpty() ? QPixmap{}
                       : QPixmap(QStringLiteral(":/overlay/%1%2.png").arg(name, suffix)));
}

void QtSoftwareKeyboardDialog::DisableKeyboardButtons() {
    const auto& key_disable_flags = initialize_parameters.key_disable_flags;
    const bool download_code = key_disable_flags.download_code != 0;

    if (page == Page::NumberPad) {
        for (const auto& keys : numberpad_buttons) {
            for (Key* key : keys) {
                key->setEnabled(!key->isHidden());
            }
        }
        return;
    }

    UpdateCharacterKeys();

    key_space->setEnabled(!key_disable_flags.space && !download_code);
    key_return->setEnabled(initialize_parameters.enable_return_button && !download_code);
    key_shift->setEnabled(!download_code);
    for (Key* tab : tab_keys) {
        tab->setEnabled(!download_code);
    }
    // Droplet has one keyboard language.
    key_globe->setEnabled(false);
}

void QtSoftwareKeyboardDialog::UpdateCharacterKeys() {
    for (const auto& keys : character_keys) {
        for (Key* key : keys) {
            key->setEnabled(!key->Label().isEmpty() && IsCharacterAllowed(key->Label().front()));
        }
    }
}

void QtSoftwareKeyboardDialog::SetBackspaceOkEnabled() {
    const auto text_length = static_cast<u32>(CurrentText().size());
    const auto position = CurrentCursorPosition();

    key_ok->setEnabled(text_length >= initialize_parameters.min_text_length);
    key_ok_num->setEnabled(text_length >= initialize_parameters.min_text_length);

    key_backspace->setEnabled(initialize_parameters.enable_backspace_button && position > 0);
    key_backspace_num->setEnabled(initialize_parameters.enable_backspace_button && position > 0);
}

bool QtSoftwareKeyboardDialog::IsCharacterAllowed(QChar character) const {
    const auto& key_disable_flags = initialize_parameters.key_disable_flags;

    if (key_disable_flags.download_code) {
        // Download codes use 0-9 and A-Z without I, O and Z.
        return character.isDigit() ||
               (character >= QLatin1Char('A') && character <= QLatin1Char('Y') &&
                character != QLatin1Char('I') && character != QLatin1Char('O'));
    }
    if (key_disable_flags.numbers && character.isDigit()) {
        return false;
    }
    if ((key_disable_flags.at || key_disable_flags.username) && character == QLatin1Char('@')) {
        return false;
    }
    if ((key_disable_flags.percent || key_disable_flags.username) &&
        character == QLatin1Char('%')) {
        return false;
    }
    if (key_disable_flags.slash && character == QLatin1Char('/')) {
        return false;
    }
    if ((key_disable_flags.backslash || key_disable_flags.username) &&
        character == QLatin1Char('\\')) {
        return false;
    }
    return true;
}

bool QtSoftwareKeyboardDialog::ValidateInputText(const QString& input_text) {
    const auto& key_disable_flags = initialize_parameters.key_disable_flags;

    const auto input_text_length = static_cast<u32>(input_text.length());

    if (input_text_length < initialize_parameters.min_text_length ||
        input_text_length > initialize_parameters.max_text_length) {
        return false;
    }

    if (key_disable_flags.space && input_text.contains(QLatin1Char{' '})) {
        return false;
    }

    if ((key_disable_flags.at || key_disable_flags.username) &&
        input_text.contains(QLatin1Char{'@'})) {
        return false;
    }

    if ((key_disable_flags.percent || key_disable_flags.username) &&
        input_text.contains(QLatin1Char{'%'})) {
        return false;
    }

    if (key_disable_flags.slash && input_text.contains(QLatin1Char{'/'})) {
        return false;
    }

    if ((key_disable_flags.backslash || key_disable_flags.username) &&
        input_text.contains(QLatin1Char('\\'))) {
        return false;
    }

    if (key_disable_flags.numbers &&
        std::any_of(input_text.begin(), input_text.end(), [](QChar c) { return c.isDigit(); })) {
        return false;
    }

    if (page == Page::NumberPad &&
        std::any_of(input_text.begin(), input_text.end(), [this](QChar c) {
            return !c.isDigit() && c != QChar{initialize_parameters.left_optional_symbol_key} &&
                   c != QChar{initialize_parameters.right_optional_symbol_key};
        })) {
        return false;
    }

    return true;
}

void QtSoftwareKeyboardDialog::SetPage(Page new_page) {
    page = new_page;
    const std::array<Page, 4> tab_pages{Page::Letters, Page::Symbols, Page::Vowels,
                                        Page::Consonants};
    for (std::size_t i = 0; i < tab_keys.size(); ++i) {
        tab_keys[i]->SetCurrent(tab_pages[i] == page);
    }
    UpdateKeyLabels();
    UpdateLegend();
}

void QtSoftwareKeyboardDialog::ChangeShift() {
    if (!shift_enabled) {
        shift_enabled = true;
    } else if (!caps_lock_enabled) {
        caps_lock_enabled = true;
    } else {
        caps_lock_enabled = false;
        shift_enabled = false;
    }
    UpdateKeyLabels();
    UpdateLegend();
}

void QtSoftwareKeyboardDialog::UpdateKeyLabels() {
    const KeyRows* rows = &LETTERS_LOWER;
    switch (page) {
    case Page::Letters:
        if (url_layout) {
            rows = shift_enabled ? &URL_UPPER : &URL_LOWER;
        } else {
            rows = shift_enabled ? &LETTERS_UPPER : &LETTERS_LOWER;
        }
        break;
    case Page::Symbols:
        rows = shift_enabled ? &SYMBOLS_SHIFT : &SYMBOLS;
        break;
    case Page::Vowels:
        rows = shift_enabled ? &VOWELS_UPPER : &VOWELS_LOWER;
        break;
    case Page::Consonants:
        rows = shift_enabled ? &CONSONANTS_UPPER : &CONSONANTS_LOWER;
        break;
    case Page::NumberPad:
        return;
    }

    for (std::size_t r = 0; r < NUM_CHARACTER_ROWS; ++r) {
        for (std::size_t c = 0; c < NUM_CHARACTER_COLUMNS; ++c) {
            character_keys[r][c]->SetLabel(QString(QChar((*rows)[r][c])));
        }
    }
    key_shift->SetShiftState(!shift_enabled      ? ShiftState::Off
                             : caps_lock_enabled ? ShiftState::Locked
                                                 : ShiftState::Once);
    UpdateCharacterKeys();
}

void QtSoftwareKeyboardDialog::NormalKeyboardButtonClicked(Key* key) {
    const auto insert = [this](const QString& text) {
        if (box_mode) {
            text_edit->insertPlainText(text);
        } else {
            line_edit->insert(text);
        }
    };

    switch (key->Role()) {
    case KeyRole::Tab: {
        const auto tab = std::find(tab_keys.begin(), tab_keys.end(), key) - tab_keys.begin();
        SetPage(std::array{Page::Letters, Page::Symbols, Page::Vowels, Page::Consonants}[tab]);
        return;
    }
    case KeyRole::Globe:
        return;
    case KeyRole::Return:
        insert(QStringLiteral("\n"));
        return;
    case KeyRole::Space:
        insert(QStringLiteral(" "));
        return;
    case KeyRole::Shift:
        ChangeShift();
        return;
    case KeyRole::Backspace:
        if (box_mode) {
            auto text_cursor = text_edit->textCursor();
            text_edit->setTextCursor(text_cursor);
            text_cursor.deletePreviousChar();
        } else {
            line_edit->backspace();
        }
        return;
    case KeyRole::Ok: {
        const auto text = CurrentText();
        auto text_str = Common::U16StringFromBuffer(text.utf16(), text.size());

        emit SubmitNormalText(SwkbdResult::Ok, std::move(text_str));
        return;
    }
    case KeyRole::Character:
        break;
    }

    insert(key->Label());

    // Revert the keyboard to lowercase if the shift key is active.
    if (page != Page::NumberPad && shift_enabled && !caps_lock_enabled) {
        shift_enabled = false;
        UpdateKeyLabels();
        UpdateLegend();
    }
}

void QtSoftwareKeyboardDialog::InlineKeyboardButtonClicked(Key* key) {
    if (!key->isEnabled()) {
        return;
    }

    switch (key->Role()) {
    case KeyRole::Tab: {
        const auto tab = std::find(tab_keys.begin(), tab_keys.end(), key) - tab_keys.begin();
        SetPage(std::array{Page::Letters, Page::Symbols, Page::Vowels, Page::Consonants}[tab]);
        return;
    }
    case KeyRole::Globe:
        return;
    case KeyRole::Return:
        InlineTextInsertString(u"\n");
        return;
    case KeyRole::Space:
        InlineTextInsertString(u" ");
        return;
    case KeyRole::Shift:
        ChangeShift();
        return;
    case KeyRole::Backspace:
        if (cursor_position <= 0 || current_text.empty()) {
            cursor_position = 0;
            return;
        }

        --cursor_position;

        current_text.erase(cursor_position, 1);

        SetBackspaceOkEnabled();
        UpdateLegend();

        emit SubmitInlineText(SwkbdReplyType::ChangedString, current_text, cursor_position);
        return;
    case KeyRole::Ok:
        emit SubmitInlineText(SwkbdReplyType::DecidedEnter, current_text, cursor_position);
        return;
    case KeyRole::Character:
        break;
    }

    const auto button_text = key->Label();
    InlineTextInsertString(Common::U16StringFromBuffer(button_text.utf16(), button_text.size()));

    // Revert the keyboard to lowercase if the shift key is active.
    if (page != Page::NumberPad && shift_enabled && !caps_lock_enabled) {
        shift_enabled = false;
        UpdateKeyLabels();
        UpdateLegend();
    }
}

void QtSoftwareKeyboardDialog::InlineTextInsertString(std::u16string_view string) {
    if ((current_text.size() + string.size()) > initialize_parameters.max_text_length) {
        return;
    }

    current_text.insert(cursor_position, string);

    cursor_position += static_cast<s32>(string.size());

    SetBackspaceOkEnabled();
    UpdateLegend();

    emit SubmitInlineText(SwkbdReplyType::ChangedString, current_text, cursor_position);
}

void QtSoftwareKeyboardDialog::KeyHovered(Key* key) {
    // A hover only counts once the mouse moves: the pointer may rest over the keys when the
    // keyboard opens.
    const bool numberpad = page == Page::NumberPad;
    const std::size_t rows = numberpad ? NUM_ROWS_NUMPAD : NUM_ROWS_NORMAL;
    const std::size_t columns = numberpad ? NUM_COLUMNS_NUMPAD : NUM_COLUMNS_NORMAL;
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t c = 0; c < columns; ++c) {
            if (GridKey(r, c) == key) {
                row = r;
                column = c;
                if (!theme->hover_enabled) {
                    theme->hover_enabled = true;
                    focus_ring->hide();
                }
                key->update();
                return;
            }
        }
    }
}

template <Core::HID::NpadButton... T>
void QtSoftwareKeyboardDialog::HandleButtonPressedOnce() {
    const auto f = [this](Core::HID::NpadButton button) {
        if (input_interpreter->IsButtonPressedOnce(button)) {
            QMetaObject::invokeMethod(
                this, [this, button] { TranslateButtonPress(button); }, Qt::QueuedConnection);
        }
    };

    (f(T), ...);
}

template <Core::HID::NpadButton... T>
void QtSoftwareKeyboardDialog::HandleButtonHold() {
    const auto f = [this](Core::HID::NpadButton button) {
        if (input_interpreter->IsButtonHeld(button)) {
            QMetaObject::invokeMethod(
                this, [this, button] { TranslateButtonPress(button); }, Qt::QueuedConnection);
        }
    };

    (f(T), ...);
}

void QtSoftwareKeyboardDialog::TranslateButtonPress(Core::HID::NpadButton button) {
    const bool numberpad = page == Page::NumberPad;
    const auto press = [](Key* key) {
        if (key != nullptr && key->isEnabled() && !key->isHidden()) {
            key->Flash();
            key->click();
        }
    };

    // Controller input brings the ring back after the mouse hid it.
    if (theme->hover_enabled) {
        theme->hover_enabled = false;
        for (Key* key : all_keys) {
            key->update();
        }
    }
    if (focus_ring->isHidden() && isVisible()) {
        focus_ring->show();
        UpdateFocusRing();
    }

    switch (button) {
    case Core::HID::NpadButton::A:
        press(GridKey(row, column));
        break;
    case Core::HID::NpadButton::B:
        press(numberpad ? key_backspace_num : key_backspace);
        break;
    case Core::HID::NpadButton::X:
        if (is_inline) {
            emit SubmitInlineText(SwkbdReplyType::DecidedCancel, current_text, cursor_position);
        } else {
            const auto text = CurrentText();
            auto text_str = Common::U16StringFromBuffer(text.utf16(), text.size());

            emit SubmitNormalText(SwkbdResult::Cancel, std::move(text_str));
        }
        break;
    case Core::HID::NpadButton::Y:
        if (!numberpad) {
            press(key_space);
        }
        break;
    case Core::HID::NpadButton::StickL:
    case Core::HID::NpadButton::StickR:
        if (!numberpad) {
            press(key_shift);
        }
        break;
    case Core::HID::NpadButton::L:
        MoveTextCursorDirection(Direction::Left);
        break;
    case Core::HID::NpadButton::R:
        MoveTextCursorDirection(Direction::Right);
        break;
    case Core::HID::NpadButton::Plus:
        press(numberpad ? key_ok_num : key_ok);
        break;
    case Core::HID::NpadButton::Left:
    case Core::HID::NpadButton::StickLLeft:
    case Core::HID::NpadButton::StickRLeft:
        MoveButtonDirection(Direction::Left);
        break;
    case Core::HID::NpadButton::Up:
    case Core::HID::NpadButton::StickLUp:
    case Core::HID::NpadButton::StickRUp:
        MoveButtonDirection(Direction::Up);
        break;
    case Core::HID::NpadButton::Right:
    case Core::HID::NpadButton::StickLRight:
    case Core::HID::NpadButton::StickRRight:
        MoveButtonDirection(Direction::Right);
        break;
    case Core::HID::NpadButton::Down:
    case Core::HID::NpadButton::StickLDown:
    case Core::HID::NpadButton::StickRDown:
        MoveButtonDirection(Direction::Down);
        break;
    default:
        break;
    }
}

QtSoftwareKeyboardDialog::Key* QtSoftwareKeyboardDialog::GridKey(std::size_t grid_row,
                                                                 std::size_t grid_column) const {
    if (page == Page::NumberPad) {
        return grid_row < NUM_ROWS_NUMPAD && grid_column < NUM_COLUMNS_NUMPAD
                   ? numberpad_buttons[grid_row][grid_column]
                   : nullptr;
    }
    return grid_row < NUM_ROWS_NORMAL && grid_column < NUM_COLUMNS_NORMAL
               ? keyboard_buttons[grid_row][grid_column]
               : nullptr;
}

void QtSoftwareKeyboardDialog::MoveButtonDirection(Direction direction) {
    const bool numberpad = page == Page::NumberPad;
    const std::size_t max_rows = numberpad ? NUM_ROWS_NUMPAD : NUM_ROWS_NORMAL;
    const std::size_t max_columns = numberpad ? NUM_COLUMNS_NUMPAD : NUM_COLUMNS_NORMAL;
    const auto usable = [](const Key* key) {
        return key != nullptr && !key->isHidden() && key->isEnabled();
    };

    // Changes the row or column index depending on the direction.
    const auto move_direction = [this, direction, max_rows, max_columns] {
        switch (direction) {
        case Direction::Left:
            column = (column + max_columns - 1) % max_columns;
            break;
        case Direction::Up:
            row = (row + max_rows - 1) % max_rows;
            break;
        case Direction::Right:
            column = (column + 1) % max_columns;
            break;
        case Direction::Down:
            row = (row + 1) % max_rows;
            break;
        default:
            break;
        }
    };
    const bool vertical = direction == Direction::Up || direction == Direction::Down;

    // Store the initial row and column.
    const auto initial_row = row;
    const auto initial_column = column;
    const Key* const prev_button = GridKey(row, column);

    for (std::size_t steps = 0; steps < max_rows * max_columns; ++steps) {
        move_direction();
        const Key* const curr_button = GridKey(row, column);
        if (curr_button != prev_button && usable(curr_button)) {
            break;
        }
        // Moving up or down onto a key that can't be used (the globe, a key the game turned
        // off), take the nearest usable key of that row instead of skipping the row.
        if (vertical && curr_button != prev_button) {
            bool found = false;
            for (std::size_t distance = 1; distance < max_columns && !found; ++distance) {
                for (const std::size_t candidate : {column + distance, column - distance}) {
                    if (candidate < max_columns && GridKey(row, candidate) != prev_button &&
                        usable(GridKey(row, candidate))) {
                        column = candidate;
                        found = true;
                        break;
                    }
                }
            }
            if (found) {
                break;
            }
        }
        // If we returned back to where we started from, stop.
        if (row == initial_row && column == initial_column) {
            break;
        }
    }

    UpdateFocusRing();
}

void QtSoftwareKeyboardDialog::MoveTextCursorDirection(Direction direction) {
    switch (direction) {
    case Direction::Left:
        if (is_inline) {
            if (cursor_position <= 0) {
                cursor_position = 0;
            } else {
                --cursor_position;
                emit SubmitInlineText(SwkbdReplyType::MovedCursor, current_text, cursor_position);
            }
        } else {
            if (box_mode) {
                text_edit->moveCursor(QTextCursor::Left);
            } else {
                line_edit->setCursorPosition(line_edit->cursorPosition() - 1);
            }
        }
        break;
    case Direction::Right:
        if (is_inline) {
            if (cursor_position >= static_cast<s32>(current_text.size())) {
                cursor_position = static_cast<s32>(current_text.size());
            } else {
                ++cursor_position;
                emit SubmitInlineText(SwkbdReplyType::MovedCursor, current_text, cursor_position);
            }
        } else {
            if (box_mode) {
                text_edit->moveCursor(QTextCursor::Right);
            } else {
                line_edit->setCursorPosition(line_edit->cursorPosition() + 1);
            }
        }
        break;
    default:
        break;
    }
    UpdateLegend();
}

void QtSoftwareKeyboardDialog::UpdateFocusRing() {
    if (focus_ring == nullptr) {
        return;
    }
    const Key* const key = GridKey(row, column);
    if (key == nullptr) {
        return;
    }
    focus_ring->Track(key->geometry(), theme->scale, theme->Px(4.0f));
    focus_ring->raise();
}

void QtSoftwareKeyboardDialog::UpdateLegend() {
    QString shift_text;
    if (page != Page::NumberPad && key_shift->isEnabled()) {
        // The stick press turns on Shift, then Caps Lock, then turns both off.
        shift_text = !shift_enabled      ? tr("Shift")
                     : caps_lock_enabled ? tr("Caps Lock Off")
                                         : tr("Caps Lock");
    }
    const int position = CurrentCursorPosition();
    panel->SetLegend(shift_text, tr("Cancel"), tr("Enter"), position > 0,
                     position < static_cast<int>(CurrentText().size()));
}

QString QtSoftwareKeyboardDialog::CurrentText() const {
    if (is_inline) {
        return QString::fromStdU16String(current_text);
    }
    return box_mode ? text_edit->toPlainText() : line_edit->text();
}

int QtSoftwareKeyboardDialog::CurrentCursorPosition() const {
    if (is_inline) {
        return cursor_position;
    }
    return box_mode ? text_edit->textCursor().position() : line_edit->cursorPosition();
}

void QtSoftwareKeyboardDialog::UpdateCharacterCount() {
    character_count = QStringLiteral("%1/%2")
                          .arg(CurrentText().size())
                          .arg(initialize_parameters.max_text_length);
    update(character_count_rect);
}

void QtSoftwareKeyboardDialog::StartInputThread() {
    // There is no controller input to read without a game (the test hook's keyboards).
    if (!input_interpreter) {
        return;
    }
    input_thread = std::jthread([&](std::stop_token stoken) {
        while (!stoken.stop_requested()) {
            input_interpreter->PollInput();
            HandleButtonPressedOnce<
                Core::HID::NpadButton::A, Core::HID::NpadButton::B, Core::HID::NpadButton::X,
                Core::HID::NpadButton::Y, Core::HID::NpadButton::StickL,
                Core::HID::NpadButton::StickR, Core::HID::NpadButton::L, Core::HID::NpadButton::R,
                Core::HID::NpadButton::Plus, Core::HID::NpadButton::Left, Core::HID::NpadButton::Up,
                Core::HID::NpadButton::Right, Core::HID::NpadButton::Down,
                Core::HID::NpadButton::StickLLeft, Core::HID::NpadButton::StickLUp,
                Core::HID::NpadButton::StickLRight, Core::HID::NpadButton::StickLDown,
                Core::HID::NpadButton::StickRLeft, Core::HID::NpadButton::StickRUp,
                Core::HID::NpadButton::StickRRight, Core::HID::NpadButton::StickRDown>();
            HandleButtonHold<Core::HID::NpadButton::B, Core::HID::NpadButton::L,
                             Core::HID::NpadButton::R, Core::HID::NpadButton::Left,
                             Core::HID::NpadButton::Up, Core::HID::NpadButton::Right,
                             Core::HID::NpadButton::Down, Core::HID::NpadButton::StickLLeft,
                             Core::HID::NpadButton::StickLUp, Core::HID::NpadButton::StickLRight,
                             Core::HID::NpadButton::StickLDown, Core::HID::NpadButton::StickRLeft,
                             Core::HID::NpadButton::StickRUp, Core::HID::NpadButton::StickRRight,
                             Core::HID::NpadButton::StickRDown>();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
}

void QtSoftwareKeyboardDialog::StopInputThread() {
    input_thread.request_stop();
    if (input_interpreter)
        input_interpreter->ResetButtonStates();
}

QtSoftwareKeyboard::QtSoftwareKeyboard(MainWindow& main_window) {
    connect(this, &QtSoftwareKeyboard::MainWindowInitializeKeyboard, &main_window,
            &MainWindow::SoftwareKeyboardInitialize, Qt::QueuedConnection);
    connect(this, &QtSoftwareKeyboard::MainWindowShowNormalKeyboard, &main_window,
            &MainWindow::SoftwareKeyboardShowNormal, Qt::QueuedConnection);
    connect(this, &QtSoftwareKeyboard::MainWindowShowTextCheckDialog, &main_window,
            &MainWindow::SoftwareKeyboardShowTextCheck, Qt::QueuedConnection);
    connect(this, &QtSoftwareKeyboard::MainWindowShowInlineKeyboard, &main_window,
            &MainWindow::SoftwareKeyboardShowInline, Qt::QueuedConnection);
    connect(this, &QtSoftwareKeyboard::MainWindowHideInlineKeyboard, &main_window,
            &MainWindow::SoftwareKeyboardHideInline, Qt::QueuedConnection);
    connect(this, &QtSoftwareKeyboard::MainWindowInlineTextChanged, &main_window,
            &MainWindow::SoftwareKeyboardInlineTextChanged, Qt::QueuedConnection);
    connect(this, &QtSoftwareKeyboard::MainWindowExitKeyboard, &main_window,
            &MainWindow::SoftwareKeyboardExit, Qt::QueuedConnection);
    connect(&main_window, &MainWindow::SoftwareKeyboardSubmitNormalText, this,
            &QtSoftwareKeyboard::SubmitNormalText, Qt::QueuedConnection);
    connect(&main_window, &MainWindow::SoftwareKeyboardSubmitInlineText, this,
            &QtSoftwareKeyboard::SubmitInlineText, Qt::QueuedConnection);
}

QtSoftwareKeyboard::~QtSoftwareKeyboard() = default;

void QtSoftwareKeyboard::InitializeKeyboard(
    bool is_inline, Core::Frontend::KeyboardInitializeParameters initialize_parameters,
    SubmitNormalCallback submit_normal_callback_, SubmitInlineCallback submit_inline_callback_) {
    if (is_inline) {
        submit_inline_callback = std::move(submit_inline_callback_);
    } else {
        submit_normal_callback = std::move(submit_normal_callback_);
    }

    LOG_INFO(Service_AM,
             "\nKeyboardInitializeParameters:"
             "\nok_text={}"
             "\nheader_text={}"
             "\nsub_text={}"
             "\nguide_text={}"
             "\ninitial_text={}"
             "\nmax_text_length={}"
             "\nmin_text_length={}"
             "\ninitial_cursor_position={}"
             "\ntype={}"
             "\npassword_mode={}"
             "\ntext_draw_type={}"
             "\nkey_disable_flags={}"
             "\nuse_blur_background={}"
             "\nenable_backspace_button={}"
             "\nenable_return_button={}"
             "\ndisable_cancel_button={}",
             Common::UTF16ToUTF8(initialize_parameters.ok_text),
             Common::UTF16ToUTF8(initialize_parameters.header_text),
             Common::UTF16ToUTF8(initialize_parameters.sub_text),
             Common::UTF16ToUTF8(initialize_parameters.guide_text),
             Common::UTF16ToUTF8(initialize_parameters.initial_text),
             initialize_parameters.max_text_length, initialize_parameters.min_text_length,
             initialize_parameters.initial_cursor_position, initialize_parameters.type,
             initialize_parameters.password_mode, initialize_parameters.text_draw_type,
             initialize_parameters.key_disable_flags.raw, initialize_parameters.use_blur_background,
             initialize_parameters.enable_backspace_button,
             initialize_parameters.enable_return_button,
             initialize_parameters.disable_cancel_button);

    emit MainWindowInitializeKeyboard(is_inline, std::move(initialize_parameters));
}

void QtSoftwareKeyboard::ShowNormalKeyboard() const {
    emit MainWindowShowNormalKeyboard();
}

void QtSoftwareKeyboard::ShowTextCheckDialog(
    Service::AM::Frontend::SwkbdTextCheckResult text_check_result,
    std::u16string text_check_message) const {
    emit MainWindowShowTextCheckDialog(text_check_result, std::move(text_check_message));
}

void QtSoftwareKeyboard::ShowInlineKeyboard(
    Core::Frontend::InlineAppearParameters appear_parameters) const {
    LOG_INFO(Service_AM,
             "\nInlineAppearParameters:"
             "\nmax_text_length={}"
             "\nmin_text_length={}"
             "\nkey_top_scale_x={}"
             "\nkey_top_scale_y={}"
             "\nkey_top_translate_x={}"
             "\nkey_top_translate_y={}"
             "\ntype={}"
             "\nkey_disable_flags={}"
             "\nkey_top_as_floating={}"
             "\nenable_backspace_button={}"
             "\nenable_return_button={}"
             "\ndisable_cancel_button={}",
             appear_parameters.max_text_length, appear_parameters.min_text_length,
             appear_parameters.key_top_scale_x, appear_parameters.key_top_scale_y,
             appear_parameters.key_top_translate_x, appear_parameters.key_top_translate_y,
             appear_parameters.type, appear_parameters.key_disable_flags.raw,
             appear_parameters.key_top_as_floating, appear_parameters.enable_backspace_button,
             appear_parameters.enable_return_button, appear_parameters.disable_cancel_button);

    emit MainWindowShowInlineKeyboard(std::move(appear_parameters));
}

void QtSoftwareKeyboard::HideInlineKeyboard() const {
    emit MainWindowHideInlineKeyboard();
}

void QtSoftwareKeyboard::InlineTextChanged(
    Core::Frontend::InlineTextParameters text_parameters) const {
    LOG_INFO(Service_AM,
             "\nInlineTextParameters:"
             "\ninput_text={}"
             "\ncursor_position={}",
             Common::UTF16ToUTF8(text_parameters.input_text), text_parameters.cursor_position);

    emit MainWindowInlineTextChanged(std::move(text_parameters));
}

void QtSoftwareKeyboard::ExitKeyboard() const {
    emit MainWindowExitKeyboard();
}

void QtSoftwareKeyboard::SubmitNormalText(Service::AM::Frontend::SwkbdResult result,
                                          std::u16string submitted_text, bool confirmed) const {
    submit_normal_callback(result, submitted_text, confirmed);
}

void QtSoftwareKeyboard::SubmitInlineText(Service::AM::Frontend::SwkbdReplyType reply_type,
                                          std::u16string submitted_text,
                                          s32 cursor_position) const {
    submit_inline_callback(reply_type, submitted_text, cursor_position);
}
