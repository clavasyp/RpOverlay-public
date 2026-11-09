#include "Theme.h"

// ---------------------------------------------------------------------------
// Активная тема. По умолчанию "graphite" (исторический вид). main.cpp вызывает
// setActive(settings::uiTheme()) до построения окон; SettingsDialog — при смене.
// ---------------------------------------------------------------------------
namespace {
QString g_active = QStringLiteral("majestic");   // дефолт — фирменная тема Majestic (design handoff)

// === Палитры (код-рендеринг: чек-лист + транскрипт) ===
const theme::Palette &graphitePalette()
{
    static const theme::Palette p{
        QColor("#5fd07a"),   // checklistOk
        QColor("#e06464"),   // checklistErr
        QColor("#9aa0aa"),   // checklistUnknown
        QColor("#e8e8ec"),   // transcriptFinal
        QColor("#888d96"),   // transcriptPartial
    };
    return p;
}

const theme::Palette &majesticPalette()
{
    // Канонические токены дизайн-системы Majestic (design_handoff_majestic_overlay).
    static const theme::Palette p{
        QColor("#5ECC3C"),   // checklistOk        — Majestic success-green
        QColor("#EA3323"),   // checklistErr       — Majestic danger-red
        QColor("#BABAC0"),   // checklistUnknown   — secondary gray
        QColor("#FFFFFF"),   // transcriptFinal    — white
        QColor("#7A7A80"),   // transcriptPartial  — muted gray
    };
    return p;
}

// === QSS: «Графит + индиго» (ВЕРБАТИМ из прежних kPanelStyle/kDialogStyle — дефолт не меняется) ===
const char *kGraphitePanel =
    "#panel { background: transparent; }"
    "#content { background: rgba(14,15,19,0.97); border: 1px solid rgba(109,94,246,0.35);"
    "           border-radius: 14px; }"
    "QLabel { color: #E8E8F0; font-family: 'Segoe UI', sans-serif; }"
    "#brandLogo { background: transparent; padding: 0; }"
    "#serverPill { color: #C9CCD6; background: rgba(255,255,255,0.05); border: 1px solid rgba(255,255,255,0.10);"
    "              border-radius: 11px; padding: 3px 10px; font-size: 11px; font-weight: 600; margin: 0; }"
    "#modePill { color: #8B7CF8; background: rgba(109,94,246,0.15); border: 1px solid rgba(109,94,246,0.55);"
    "            border-radius: 11px; padding: 3px 10px; font-size: 11px; font-weight: 700; margin: 0; }"
    "#title { font-size: 14px; font-weight: 700; color:#FFFFFF; padding: 10px 12px;"
    "         background: rgba(109,94,246,0.12);"
    "         border-top-left-radius: 13px; border-top-right-radius: 13px; }"
    "#sectionLabel { color: #9AA0AE; font-size: 10px; font-weight: 700; letter-spacing: 0.6px;"
    "                padding: 6px 12px 2px 12px; }"
    "#serverLabel { color: #8B7CF8; font-size: 12px; font-weight: 600; padding: 2px 12px; }"
    "#statusLabel { color: #9AA0AE; font-size: 11px; padding: 7px 12px;"
    "               background: rgba(255,255,255,0.03); border-top: 1px solid rgba(255,255,255,0.05);"
    "               border-bottom-left-radius: 13px; border-bottom-right-radius: 13px; }"
    "QComboBox { background: rgba(255,255,255,0.05); color: #E8E8F0; border: 1px solid rgba(255,255,255,0.08);"
    "            border-radius: 10px; padding: 7px 10px; margin: 0 12px; }"
    "QComboBox:hover { border: 1px solid rgba(109,94,246,0.55); }"
    "QComboBox::drop-down { border: none; width: 18px; }"
    "QComboBox QAbstractItemView { background: #15161C; color: #E8E8F0; border: 1px solid rgba(109,94,246,0.40);"
    "            border-radius: 8px; selection-background-color: #6D5EF6; outline: 0; padding: 4px; }"
    "QPushButton { background: #6D5EF6; color: #FFFFFF; border: none; border-radius: 11px;"
    "              padding: 9px 12px; margin: 6px 12px; font-weight: 700; }"
    "QPushButton:hover { background: #8B7CF8; }"
    "QPushButton:pressed { background: #5B4DE0; }"
    "QPushButton#listenOn { background: #E0556B; }"
    "QPushButton#listenOn:hover { background: #EC6B80; }"
    "QPushButton#settingsBtn { background: rgba(255,255,255,0.06); color: #E8E8F0; border: 1px solid"
    "                          rgba(255,255,255,0.10); border-radius: 8px; padding: 0; margin: 0; font-size: 14px; }"
    "QPushButton#settingsBtn:hover { background: rgba(109,94,246,0.30); border-color: rgba(109,94,246,0.6); }"
    "QTextEdit { background: rgba(255,255,255,0.04); color: #E8E8F0; border: 1px solid rgba(255,255,255,0.06);"
    "            border-radius: 12px; margin: 0 12px; padding: 6px; }"
    "QListWidget { background: rgba(255,255,255,0.04); color: #E8E8F0; border: 1px solid rgba(255,255,255,0.06);"
    "              border-radius: 12px; margin: 0 12px; padding: 2px; }"
    "QListWidget::item { padding: 4px 6px; border-radius: 6px; }"
    "QProgressBar#vuBar { background: rgba(0,0,0,0.35); border: 1px solid rgba(255,255,255,0.08);"
    "                     border-radius: 6px; }"
    "QProgressBar#vuBar::chunk { background: #6D5EF6; border-radius: 5px; }"
    "#linkPanel { background: rgba(16,17,24,0.98); border: 1px solid rgba(109,94,246,0.55); border-radius: 14px; }"
    "#linkCode { color: #8B7CF8; font-size: 24px; font-weight: 800; letter-spacing: 2px; padding: 6px; }"
    "#chip { color: #CFC9FF; background: rgba(109,94,246,0.22); border: 1px solid rgba(139,124,248,0.5);"
    "        border-radius: 11px; padding: 2px 9px; font-size: 11px; }"
    "#pttHint { color: #C9CCD6; font-size: 12px; font-weight: 600; padding: 10px 12px; margin: 6px 12px;"
    "           border: 1px solid rgba(109,94,246,0.35); border-radius: 12px; background: rgba(109,94,246,0.10); }"
    "#pttHintRec { color: #FFFFFF; font-size: 12px; font-weight: 700; padding: 10px 12px; margin: 6px 12px;"
    "             border: 1px solid rgba(224,85,107,0.6); border-radius: 12px; background: rgba(224,85,107,0.22); }"
    "#collapsedBar { background: rgba(14,15,19,0.97); border: 1px solid rgba(109,94,246,0.45); border-radius: 16px; }"
    "#pillStatus { color: #C9CCD6; font-size: 12px; }"
    "QPushButton#pillBtn { background: rgba(255,255,255,0.06); color: #E8E8F0; border: 1px solid"
    "                      rgba(255,255,255,0.12); border-radius: 12px; padding: 0; margin: 0; font-size: 14px; }"
    "QPushButton#pillBtn:hover { background: rgba(109,94,246,0.30); }"
    "QPushButton#pillBtnOn { background: #E0556B; color: #FFFFFF; border: none; border-radius: 12px;"
    "                        padding: 0; margin: 0; font-size: 14px; }"
    "QPushButton#pillMic { background: #6D5EF6; color: #FFFFFF; border: none; border-radius: 12px;"
    "                      padding: 0; margin: 0; font-size: 14px; }"
    "QPushButton#pillMic:hover { background: #8B7CF8; }"
    "QPushButton#pillMicOn { background: #E0556B; color: #FFFFFF; border: none; border-radius: 12px;"
    "                        padding: 0; margin: 0; font-size: 14px; }";

const char *kGraphiteDialog =
    // Окно прозрачно — карточку рисует #dlgContent (frameless + WA_TranslucentBackground).
    "QDialog { background: transparent; }"
    // Прокрутка страницы (страницы обёрнуты в QScrollArea, иначе высокие блоки давят вёрстку).
    // Сама область и её виджет — прозрачные: фон рисует карточка #dlgContent под ними.
    "#dlgScroll { background: transparent; border: none; }"
    "#dlgScroll > QWidget > QWidget { background: transparent; }"
    "#dlgScroll QScrollBar:vertical { background: transparent; width: 8px; margin: 2px 0; }"
    "#dlgScroll QScrollBar::handle:vertical { background: rgba(255,255,255,0.16); border-radius: 4px;"
    "                                         min-height: 28px; }"
    "#dlgScroll QScrollBar::handle:vertical:hover { background: rgba(109,94,246,0.55); }"
    "#dlgScroll QScrollBar::add-line:vertical, #dlgScroll QScrollBar::sub-line:vertical { height: 0; }"
    "#dlgScroll QScrollBar::add-page:vertical, #dlgScroll QScrollBar::sub-page:vertical"
    "                                         { background: transparent; }"
    "QLabel { color: #E8E8F0; font-family: 'Segoe UI', sans-serif; }"
    // Плавающая карточка + шапка/футер/сайдбар.
    "#dlgContent { background: #0E0F13; border: 1px solid rgba(109,94,246,0.40); border-radius: 14px; }"
    "#dlgHead { background: rgba(109,94,246,0.12); border-bottom: 2px solid #6D5EF6;"
    "           border-top-left-radius: 13px; border-top-right-radius: 13px; }"
    "#dlgBrandLogo { background: transparent; padding: 0; }"
    "#dlgTitle { font-size: 14px; font-weight: 700; color: #FFFFFF; background: transparent; }"
    "#dlgSide { background: rgba(255,255,255,0.02); border-right: 1px solid rgba(255,255,255,0.07); }"
    "#dlgNav { background: transparent; border: none; outline: 0; }"
    "#dlgNav::item { color: #9AA0AE; padding: 7px 11px; border-radius: 8px; margin: 2px 0; }"
    "#dlgNav::item:hover { background: rgba(255,255,255,0.05); color: #E8E8F0; }"
    "#dlgNav::item:selected { background: rgba(109,94,246,0.22); border: 1px solid rgba(139,124,248,0.5);"
    "                         color: #FFFFFF; }"
    "#dlgVer { color: #6A6F7A; font-size: 10px; padding: 6px 6px 2px 6px; }"
    "#dlgSection { color: #9AA0AE; font-size: 10px; font-weight: 800; letter-spacing: 1px;"
    "              padding-left: 9px; border-left: 3px solid #6D5EF6; }"
    "#dlgField { color: #9AA0AE; font-size: 12px; font-weight: 600; }"
    "#dlgSliderVal { color: #E8E8F0; font-size: 12px; font-weight: 700; }"
    "#dlgKeyCap { color: #E8E8F0; background: rgba(255,255,255,0.06); border: 1px solid rgba(255,255,255,0.12);"
    "             border-bottom-width: 2px; border-radius: 5px; padding: 3px 10px; font-size: 11px; font-weight: 700; }"
    "#dlgHint { color: #8A8F99; font-size: 11px; }"
    "#dlgFoot { background: rgba(255,255,255,0.03); border-top: 1px solid rgba(255,255,255,0.06);"
    "           border-bottom-left-radius: 13px; border-bottom-right-radius: 13px; }"
    "#dlgServerPill { color: #C9CCD6; background: rgba(255,255,255,0.06); border: 1px solid rgba(255,255,255,0.12);"
    "                 border-radius: 14px; padding: 4px 12px; font-size: 12px; font-weight: 700; }"
    "QComboBox { background: rgba(255,255,255,0.05); color: #E8E8F0; border: 1px solid rgba(255,255,255,0.08);"
    "            border-radius: 8px; padding: 7px 10px; min-height: 18px; }"
    "QComboBox:hover { border: 1px solid rgba(109,94,246,0.55); }"
    "QComboBox:focus { border: 1px solid rgba(109,94,246,0.55); }"
    "QComboBox::drop-down { border: none; width: 18px; }"
    "QComboBox QAbstractItemView { background: #15161C; color: #E8E8F0; border: 1px solid rgba(109,94,246,0.40);"
    "            border-radius: 8px; selection-background-color: #6D5EF6; outline: 0; padding: 4px; }"
    "QKeySequenceEdit { background: rgba(255,255,255,0.05); color: #E8E8F0;"
    "            border: 1px solid rgba(255,255,255,0.08); border-radius: 6px; padding: 6px 8px; }"
    "QKeySequenceEdit:focus { border: 1px solid rgba(109,94,246,0.55); }"
    "QSlider::groove:horizontal { height: 6px; background: rgba(255,255,255,0.10); border-radius: 3px; }"
    "QSlider::handle:horizontal { background: #8B7CF8; width: 16px; margin: -6px 0; border-radius: 8px; }"
    "QSlider::sub-page:horizontal { background: #6D5EF6; border-radius: 3px; }"
    "QPushButton { background: #6D5EF6; color: #FFFFFF; border: none; border-radius: 8px;"
    "              padding: 8px 16px; font-weight: 700; }"
    "QPushButton:hover { background: #8B7CF8; }"
    "QPushButton:pressed { background: #5B4DE0; }"
    "QPushButton#secondary { background: rgba(255,255,255,0.08); color: #E8E8F0; border: 1px solid rgba(255,255,255,0.12); }"
    "QPushButton#secondary:hover { background: rgba(109,94,246,0.30); border-color: rgba(109,94,246,0.6); }"
    "QPushButton#winClose { background: rgba(255,255,255,0.06); color: #E8E8F0; border: 1px solid rgba(255,255,255,0.12);"
    "                       border-radius: 6px; padding: 0; font-size: 13px; }"
    "QPushButton#winClose:hover { background: rgba(109,94,246,0.30); border-color: rgba(109,94,246,0.6); }"
    "QCheckBox { color: #E8E8F0; font-family: 'Segoe UI', sans-serif; font-size: 12px; }"
    "QCheckBox::indicator { width: 16px; height: 16px; border-radius: 4px;"
    "                       border: 1px solid rgba(255,255,255,0.12); background: rgba(255,255,255,0.05); }"
    "QCheckBox::indicator:checked { background: #6D5EF6; border: 1px solid #6D5EF6; }"
    "QProgressBar#vuBar { background: rgba(0,0,0,0.35); border: 1px solid rgba(255,255,255,0.08);"
    "                     border-radius: 6px; }"
    "QProgressBar#vuBar::chunk { background: #6D5EF6; border-radius: 5px; }";

// === QSS: «Majestic» — фирменный стиль majestic-rp.ru (извлечён из их прод-CSS) ===
// Палитра: страница #0E0E0E, карточки #131313/#1D1D1D, границы #2E2E2E, акцент-роза #E81C5A
// (--accent-fill), success #5ECC3C, danger #EA3323. Розовое «свечение» (box-shadow в их вебе)
// QSS не умеет; эффект тени из кода убран в 0.3.0 (подозревался в крашах Qt6Gui).
const char *kMajesticPanel =
    "#panel { background: transparent; }"
    // Плавающая карточка: скруглённый near-black контейнер + тонкая crimson-рамка
    // (без программного «свечения» — эффект тени убран в 0.3.0).
    "#content { background: #0E0E0E; border: 1px solid #E81C5A; border-radius: 14px; }"
    "QLabel { color: #FFFFFF; font-family: 'Onest','Segoe UI', sans-serif; }"
    "#brandLogo { background: transparent; padding: 0; }"
    // Пилюли «сервер» / «режим».
    "#serverPill { color: #D7D7D7; background: #131313; border: 1px solid #2E2E2E;"
    "              border-radius: 11px; padding: 3px 10px; font-size: 11px; font-weight: 600; margin: 0; }"
    "#modePill { color: #E81C5A; background: rgba(232,28,90,0.12); border: 1px solid rgba(232,28,90,0.55);"
    "            border-radius: 11px; padding: 3px 10px; font-size: 11px; font-weight: 700; margin: 0; }"
    // Header: flat dark bar with the crimson underline.
    "#title { padding: 10px 12px; color:#FFFFFF; font-size:14px; font-weight:700;"
    "         background: #131313; border-bottom: 2px solid #E81C5A;"
    "         border-top-left-radius: 13px; border-top-right-radius: 13px; }"
    // Section labels: tiny, heavy, gray + 3px crimson left tick.
    "#sectionLabel { color: #BABAC0; font-size: 10px; font-weight: 800; letter-spacing: 1px;"
    "                padding: 6px 12px 3px 11px; border-left: 3px solid #E81C5A; margin-left: 12px; }"
    "#serverLabel { color: #E81C5A; font-size: 12px; font-weight: 700; padding: 2px 12px; }"
    "#statusLabel { color: #BABAC0; font-size: 11px; padding: 7px 12px;"
    "               background: #131313; border-top: 1px solid #2E2E2E;"
    "               border-bottom-left-radius: 13px; border-bottom-right-radius: 13px; }"
    // Inputs / comboboxes.
    "QComboBox { background: #1D1D1D; color: #FFFFFF; border: 1px solid #2E2E2E;"
    "            border-radius: 6px; padding: 7px 10px; margin: 0 12px; min-height: 18px; }"
    "QComboBox:hover { border: 1px solid #E81C5A; }"
    "QComboBox:focus { border: 1px solid #E81C5A; }"
    "QComboBox::drop-down { border: none; width: 18px; }"
    "QComboBox QAbstractItemView { background: #181818; color: #FFFFFF; border: 1px solid #2E2E2E;"
    "            border-radius: 6px; selection-background-color: #E81C5A; outline: 0; padding: 4px; }"
    // Primary action button (crimson).
    "QPushButton { background: #E81C5A; color: #FFFFFF; border: none; border-radius: 6px;"
    "              padding: 9px 12px; margin: 6px 12px; font-weight: 700; }"
    "QPushButton:hover { background: #ED3E73; }"
    "QPushButton:pressed { background: #C9134B; }"
    "QPushButton:disabled { background: #E81C5A; color: rgba(255,255,255,0.45); }"
    // Recording / "stop listening" state — deep crimson.
    "QPushButton#listenOn { background: #B3133F; }"
    "QPushButton#listenOn:hover { background: #C9134B; }"
    // Settings gear — dark secondary.
    "QPushButton#settingsBtn { background: #1D1D1D; color: #FFFFFF; border: 1px solid #2E2E2E;"
    "                          border-radius: 6px; padding: 0; margin: 0; font-size: 14px; }"
    "QPushButton#settingsBtn:hover { background: rgba(232,28,90,0.28); border-color: #E81C5A; }"
    // Transcript / advice text areas.
    "QTextEdit { background: #131313; color: #FFFFFF; border: 1px solid #2E2E2E;"
    "            border-radius: 8px; margin: 0 12px; padding: 7px; selection-background-color: #E81C5A; }"
    // Checklist.
    "QListWidget { background: #131313; color: #FFFFFF; border: 1px solid #2E2E2E;"
    "              border-radius: 8px; margin: 0 12px; padding: 3px; }"
    "QListWidget::item { padding: 4px 6px; border-radius: 4px; }"
    "QListWidget::item:selected { background: rgba(232,28,90,0.20); color: #FFFFFF; }"
    // VU meter.
    "QProgressBar#vuBar { background: #0A0A0A; border: 1px solid #2E2E2E; border-radius: 6px; }"
    "QProgressBar#vuBar::chunk { background: #E81C5A; border-radius: 5px; }"
    // Device-login panel (glow approximated by crimson border; glow added in code).
    "#linkPanel { background: #131313; border: 1px solid #E81C5A; border-radius: 12px; }"
    "#linkCode { color: #E81C5A; font-size: 24px; font-weight: 800; letter-spacing: 2px; padding: 6px;"
    "            font-family: 'JetBrains Mono','Cascadia Code',monospace; }"
    // Article / status chips.
    "#chip { color: #FFFFFF; background: rgba(232,28,90,0.20); border: 1px solid rgba(232,28,90,0.55);"
    "        border-radius: 6px; padding: 2px 9px; font-size: 11px; }"
    // Push-to-talk hint (idle / recording).
    "#pttHint { color: #D7D7D7; font-size: 12px; font-weight: 600; padding: 10px 12px; margin: 6px 12px;"
    "           border: 1px solid #2E2E2E; border-radius: 8px; background: #131313; }"
    "#pttHintRec { color: #FFFFFF; font-size: 12px; font-weight: 700; padding: 10px 12px; margin: 6px 12px;"
    "             border: 1px solid #B3133F; border-radius: 8px; background: rgba(232,28,90,0.22); }"
    // Collapsed one-line pill.
    "#collapsedBar { background: #0E0E0E; border: 1px solid rgba(232,28,90,0.55); border-radius: 16px; }"
    "#pillStatus { color: #D7D7D7; font-size: 12px; }"
    "QPushButton#pillBtn { background: #1D1D1D; color: #FFFFFF; border: 1px solid #2E2E2E;"
    "                      border-radius: 8px; padding: 0; margin: 0; font-size: 14px; }"
    "QPushButton#pillBtn:hover { background: rgba(232,28,90,0.28); }"
    "QPushButton#pillBtnOn { background: #B3133F; color: #FFFFFF; border: none; border-radius: 8px;"
    "                        padding: 0; margin: 0; font-size: 14px; }"
    // Микрофон в «таблетке» — фирменно-crimson (idle) / глубокий crimson (запись).
    "QPushButton#pillMic { background: #E81C5A; color: #FFFFFF; border: none; border-radius: 8px;"
    "                      padding: 0; margin: 0; font-size: 14px; }"
    "QPushButton#pillMic:hover { background: #ED3E73; }"
    "QPushButton#pillMicOn { background: #B3133F; color: #FFFFFF; border: none; border-radius: 8px;"
    "                        padding: 0; margin: 0; font-size: 14px; }";

const char *kMajesticDialog =
    // Окно прозрачно — карточку рисует #dlgContent (frameless + WA_TranslucentBackground).
    "QDialog { background: transparent; }"
    // Прокрутка страницы (страницы обёрнуты в QScrollArea, иначе высокие блоки давят вёрстку).
    "#dlgScroll { background: transparent; border: none; }"
    "#dlgScroll > QWidget > QWidget { background: transparent; }"
    "#dlgScroll QScrollBar:vertical { background: transparent; width: 8px; margin: 2px 0; }"
    "#dlgScroll QScrollBar::handle:vertical { background: #2E2E2E; border-radius: 4px; min-height: 28px; }"
    "#dlgScroll QScrollBar::handle:vertical:hover { background: #E81C5A; }"
    "#dlgScroll QScrollBar::add-line:vertical, #dlgScroll QScrollBar::sub-line:vertical { height: 0; }"
    "#dlgScroll QScrollBar::add-page:vertical, #dlgScroll QScrollBar::sub-page:vertical"
    "                                         { background: transparent; }"
    "QLabel { color: #FFFFFF; font-family: 'Onest','Segoe UI', sans-serif; }"
    // Плавающая crimson-карточка.
    "#dlgContent { background: #0E0E0E; border: 1px solid #E81C5A; border-radius: 14px; }"
    // Шапка: surface + crimson-подчёркивание, скруглённый верх.
    "#dlgHead { background: #131313; border-bottom: 2px solid #E81C5A;"
    "           border-top-left-radius: 13px; border-top-right-radius: 13px; }"
    "#dlgBrandLogo { background: transparent; padding: 0; }"
    "#dlgTitle { font-size: 14px; font-weight: 700; color: #FFFFFF; background: transparent; }"
    // Сайдбар-навигация.
    "#dlgSide { background: #101011; border-right: 1px solid #2E2E2E; }"
    "#dlgNav { background: transparent; border: none; outline: 0; }"
    "#dlgNav::item { color: #BABAC0; padding: 7px 11px; border-radius: 8px; margin: 2px 0; }"
    "#dlgNav::item:hover { background: rgba(255,255,255,0.05); color: #FFFFFF; }"
    "#dlgNav::item:selected { background: rgba(232,28,90,0.20); border: 1px solid rgba(232,28,90,0.55);"
    "                         color: #FFFFFF; }"
    "#dlgVer { color: #7A7A80; font-size: 10px; padding: 6px 6px 2px 6px; }"
    // Crimson-заголовок секции: серый текст + 3px crimson-тик (как #sectionLabel в оверлее).
    "#dlgSection { color: #BABAC0; font-size: 10px; font-weight: 800; letter-spacing: 1px;"
    "              padding-left: 9px; border-left: 3px solid #E81C5A; }"
    "#dlgField { color: #BABAC0; font-size: 12px; font-weight: 600; }"
    "#dlgSliderVal { color: #FFFFFF; font-size: 12px; font-weight: 700; }"
    // PTT-биндинг как кейкап.
    "#dlgKeyCap { color: #FFFFFF; background: #1D1D1D; border: 1px solid #2E2E2E; border-bottom-width: 2px;"
    "             border-radius: 5px; padding: 3px 10px; font-size: 11px; font-weight: 700; }"
    "#dlgHint { color: #7A7A80; font-size: 11px; }"
    // Футер: surface + верхний hairline, скруглённый низ.
    "#dlgFoot { background: #131313; border-top: 1px solid #2E2E2E;"
    "           border-bottom-left-radius: 13px; border-bottom-right-radius: 13px; }"
    "#dlgServerPill { color: #D7D7D7; background: #1D1D1D; border: 1px solid #2E2E2E;"
    "                 border-radius: 14px; padding: 4px 12px; font-size: 12px; font-weight: 700; }"
    // Inputs.
    "QComboBox { background: #1D1D1D; color: #FFFFFF; border: 1px solid #2E2E2E;"
    "            border-radius: 6px; padding: 7px 10px; min-height: 18px; }"
    "QComboBox:hover { border: 1px solid #E81C5A; }"
    "QComboBox:focus { border: 1px solid #E81C5A; }"
    "QComboBox::drop-down { border: none; width: 18px; }"
    "QComboBox QAbstractItemView { background: #181818; color: #FFFFFF; border: 1px solid #2E2E2E;"
    "            border-radius: 6px; selection-background-color: #E81C5A; outline: 0; padding: 4px; }"
    "QKeySequenceEdit { background: #1D1D1D; color: #FFFFFF;"
    "            border: 1px solid #2E2E2E; border-radius: 6px; padding: 6px 8px; }"
    "QKeySequenceEdit:focus { border: 1px solid #E81C5A; }"
    // Sliders — crimson fill + lighter crimson handle.
    "QSlider::groove:horizontal { height: 6px; background: #2E2E2E; border-radius: 3px; }"
    "QSlider::handle:horizontal { background: #ED3E73; width: 16px; margin: -6px 0; border-radius: 8px; }"
    "QSlider::sub-page:horizontal { background: #E81C5A; border-radius: 3px; }"
    // Buttons.
    "QPushButton { background: #E81C5A; color: #FFFFFF; border: none; border-radius: 6px;"
    "              padding: 8px 16px; font-weight: 700; }"
    "QPushButton:hover { background: #ED3E73; }"
    "QPushButton:pressed { background: #C9134B; }"
    "QPushButton#secondary { background: #1D1D1D; color: #FFFFFF; border: 1px solid #2E2E2E; }"
    "QPushButton#secondary:hover { background: rgba(232,28,90,0.28); border-color: #E81C5A; }"
    "QPushButton#winClose { background: #1D1D1D; color: #FFFFFF; border: 1px solid #2E2E2E;"
    "                       border-radius: 6px; padding: 0; font-size: 13px; }"
    "QPushButton#winClose:hover { background: rgba(232,28,90,0.28); border-color: #E81C5A; }"
    // Checkbox — crimson when checked.
    "QCheckBox { color: #FFFFFF; font-family: 'Onest','Segoe UI', sans-serif; font-size: 12px; }"
    "QCheckBox::indicator { width: 16px; height: 16px; border-radius: 4px;"
    "                       border: 1px solid #2E2E2E; background: #1D1D1D; }"
    "QCheckBox::indicator:checked { background: #E81C5A; border: 1px solid #E81C5A; }"
    "QProgressBar#vuBar { background: #0A0A0A; border: 1px solid #2E2E2E; border-radius: 6px; }"
    "QProgressBar#vuBar::chunk { background: #E81C5A; border-radius: 5px; }";

} // namespace

namespace theme {

void setActive(const QString &name)
{
    g_active = (name == QLatin1String("majestic")) ? QStringLiteral("majestic")
                                                    : QStringLiteral("graphite");
}

QString active() { return g_active; }

bool isMajestic() { return g_active == QLatin1String("majestic"); }

const Palette &palette()
{
    return isMajestic() ? majesticPalette() : graphitePalette();
}

QString panelStyle()
{
    return QString::fromUtf8(isMajestic() ? kMajesticPanel : kGraphitePanel);
}

QString dialogStyle()
{
    return QString::fromUtf8(isMajestic() ? kMajesticDialog : kGraphiteDialog);
}

} // namespace theme
