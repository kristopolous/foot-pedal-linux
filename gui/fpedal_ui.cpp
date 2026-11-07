/*
 * fpedal-ui — native X11 (Qt5) graphical programmer for the
 * PCsensor FS3-P USB foot switch (1a86:e026).
 *
 * A thin frontend over the `fpedal` CLI: all device I/O (read, safe
 * read-modify-write of the full 3-pedal image, read-back verification)
 * happens inside fpedal. This program provides:
 *
 *   - a card per pedal showing what is stored on the device
 *   - "press the combo" capture: while capturing, the app grabs the
 *     keyboard on a second X connection and listens to raw X key events,
 *     so Ctrl/Alt/Win combos are captured reliably (a focused Qt widget
 *     alone cannot always see them)
 *   - mouse buttons / relative motion / string / clear controls that
 *     edit the spec line
 *   - a live log of X key and button events this window receives
 *     (focus the window and stomp a pedal to see what X actually gets)
 *
 * Build: see Makefile target `gui`. Needs Qt5Widgets + libX11/libXtst.
 * MIT license.
 */
#include <QtWidgets>
#include <QDir>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTimer>
#include <QKeyEvent>
#include <functional>

extern "C" {
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
}
/* Xlib.h #defines these event constants; Qt uses the same names. Keep
 * usable C++ constants for the Xlib API, let Qt's enum members win. */
#undef KeyPress
#undef KeyRelease
#undef ButtonPress
#undef ButtonRelease
namespace xev { constexpr int KeyPress = 2; constexpr int KeyRelease = 3; }

#include "keymap_names.h"  /* generated from vendor/common.c by the Makefile */

/* ---------------- spec / key helpers ---------------- */

static const QStringList kModOrder = {
    "l_ctrl", "l_shift", "l_alt", "l_win", "r_ctrl", "r_shift", "r_alt", "r_win"
};

static QSet<QString> buildKeySet() {
    QSet<QString> s;
    for (const char *const *n = kKeymapNames; *n; ++n)
        s.insert(QString::fromUtf8(*n).toLower());
    return s;
}
static const QSet<QString> kKeys = buildKeySet();

static bool modKeysym(KeySym ks, QString &tok) {
    switch (ks) {
    case XK_Control_L: tok = "l_ctrl";  return true;
    case XK_Control_R: tok = "r_ctrl";  return true;
    case XK_Shift_L:   tok = "l_shift"; return true;
    case XK_Shift_R:   tok = "r_shift"; return true;
    case XK_Alt_L: case XK_Meta_L: tok = "l_alt"; return true;
    case XK_Alt_R: case XK_Meta_R: tok = "r_alt"; return true;
    case XK_Super_L:   tok = "l_win";   return true;
    case XK_Super_R:   tok = "r_win";   return true;
    default: return false;
    }
}

/* XKeysymToString() returns legacy names for punctuation; the pedal keymap
 * uses the characters themselves. Map the legacy ones back to characters. */
static QString legacyToChar(const QString &n) {
    static const QHash<QString, QString> m = {
        {"bracketleft", "["}, {"bracketright", "]"}, {"backslash", "\\"},
        {"comma", ","}, {"period", "."}, {"slash", "/"}, {"semicolon", ";"},
        {"apostrophe", "'"}, {"grave", "`"}, {"minus", "-"}, {"equal", "="},
        {"plus", "+"}, {"braceleft", "{"}, {"braceright", "}"}, {"bar", "|"},
        {"colon", ":"}, {"quotedbl", "\""}, {"asciitilde", "~"},
        {"asciicircum", "^"}, {"underscore", "_"}, {"less", "<"},
        {"greater", ">"}, {"question", "?"}, {"numbersign", "#"},
        {"dollar", "$"}, {"percent", "%"}, {"ampersand", "&"},
        {"asterisk", "*"}, {"parenleft", "("}, {"parenright", ")"},
    };
    return m.value(n, n);
}

/* token for the main (non-modifier) key of a key press, group 0 without
 * shift so the capture reflects physical keys: Shift+a is captured as
 * l_shift+a, exactly what the pedal must emit for a capital A. */
static QString keyToken(const XKeyEvent &ev) {
    KeySym ks = XLookupKeysym(&const_cast<XKeyEvent&>(ev), 0);
    QString s = legacyToChar(QString::fromUtf8(XKeysymToString(ks)));
    if (s == " ") s = "space";
    return s;
}

static QString modDisplay(const QString &tok) {
    static const QHash<QString, QString> d = {
        {"l_ctrl", "Ctrl"}, {"l_shift", "Shift"}, {"l_alt", "Alt"}, {"l_win", "Win"},
        {"r_ctrl", "R-Ctrl"}, {"r_shift", "R-Shift"}, {"r_alt", "R-Alt"}, {"r_win", "R-Win"},
    };
    return d.value(tok, tok);
}

static QString buildSpec(const QSet<QString> &mods, const QString &key) {
    QStringList parts;
    for (const QString &m : kModOrder)
        if (mods.contains(m)) parts << m;
    if (!key.isEmpty()) parts << key;
    return parts.join('+');
}

/* spec tokens are matched case-insensitively by fpedal (strcasecmp) */
static bool isValidKeyToken(const QString &t) {
    return kKeys.contains(t.toLower());
}

/* ---------------- fpedal runner ---------------- */

struct ProcResult { int code; QString out, err; };

static ProcResult runFpedal(const QString &bin, const QStringList &args) {
    QProcess p;
    p.start(bin, args);
    if (!p.waitForStarted(5000))
        return {-1, QString(), "could not start " + bin};
    if (!p.waitForFinished(25000)) {
        p.kill();
        p.waitForFinished(1000);
        return {-1, QString(), bin + " timed out"};
    }
    return {p.exitCode(), QString::fromUtf8(p.readAllStandardOutput()),
            QString::fromUtf8(p.readAllStandardError())};
}

static QJsonObject firstJsonLine(const QString &out) {
    for (const QString &line : out.split('\n')) {
        QString t = line.trimmed();
        if (t.startsWith('{') && t.endsWith('}'))
            return QJsonDocument::fromJson(t.toUtf8()).object();
    }
    return {};
}

/* ---------------- keyboard capture ---------------- */

class KeyCapture : public QObject {
public:
    std::function<void(bool ok, const QString &spec, const QString &note)> done;
    KeyCapture(QWidget *host, QLabel *status) : host_(host), status_(status) {}

    void start() {
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) { finish(false, QString(), "cannot open X display"); return; }
        XSetErrorHandler([](Display *, XErrorEvent *) { return 0; });
        int rc = XGrabKeyboard(dpy_, host_->window()->winId(), True,
                               GrabModeAsync, GrabModeAsync, CurrentTime);
        if (rc != GrabSuccess) {
            XCloseDisplay(dpy_); dpy_ = nullptr;
            finish(false, QString(), "keyboard grab failed (another app grabbed it?)");
            return;
        }
        grab_ = true;
        status_->setText("press the combo on your keyboard (Esc cancels)");
        pump_ = new QTimer(this);
        connect(pump_, &QTimer::timeout, this, [this] { pumpX(); });
        pump_->start(10);
        QTimer::singleShot(15000, this, [this] {
            if (!done_) finish(false, QString(), "capture timed out");
        });
    }

private:
    void pumpX() {
        if (!dpy_) return;
        while (XPending(dpy_)) {
            XEvent ev;
            XNextEvent(dpy_, &ev);
            if (ev.type == xev::KeyPress) onKey(ev.xkey, true);
            else if (ev.type == xev::KeyRelease) onKey(ev.xkey, false);
            if (done_) return;
        }
    }

    void onKey(const XKeyEvent &ev, bool press) {
        KeySym ks = XLookupKeysym(&const_cast<XKeyEvent&>(ev), 0);
        if (press && ks == XK_Escape) { finish(false, QString(), "capture cancelled"); return; }

        QString mod;
        if (modKeysym(ks, mod)) {
            if (press) {
                modsDown_.insert(mod);
                windowMods_.insert(mod);
            } else {
                modsDown_.remove(mod);
                /* modifier-only combo: last modifier released, no main key */
                if (modsDown_.isEmpty() && !gotMain_ && !windowMods_.isEmpty())
                    spec_ = buildSpec(windowMods_, QString());
            }
            QStringList held;
            for (const QString &m : kModOrder)
                if (modsDown_.contains(m)) held << modDisplay(m);
            if (!modsDown_.isEmpty())
                status_->setText("holding: " + held.join(" + ") + " …");
        } else if (press && !gotMain_) {
            gotMain_ = true;
            mainCode_ = ev.keycode;
            QString tok = keyToken(ev);
            if (!isValidKeyToken(tok)) {
                finish(false, QString(),
                       "key '" + tok + "' is not in the pedal keymap — try another key");
                return;
            }
            spec_ = buildSpec(modsDown_, tok);
            status_->setText("captured " + spec_ + " — release all keys");
        } else if (!press && gotMain_ && ev.keycode == mainCode_) {
            mainUp_ = true;
        }

        /* complete once everything that built the spec is released */
        if (!done_ && !spec_.isEmpty() && modsDown_.isEmpty() &&
            (!gotMain_ || mainUp_))
            finish(true, spec_, QString());
    }

    void finish(bool ok, const QString &spec, const QString &note) {
        if (done_) return;
        done_ = true;
        if (pump_) pump_->stop();
        if (grab_ && dpy_) {
            XUngrabKeyboard(dpy_, CurrentTime);
            XSync(dpy_, False);
        }
        if (dpy_) { XCloseDisplay(dpy_); dpy_ = nullptr; }
        status_->setText(ok ? "captured: " + spec : note);
        if (done) done(ok, spec, note);
    }

    QWidget *host_;
    QLabel *status_;
    Display *dpy_ = nullptr;
    QTimer *pump_ = nullptr;
    bool grab_ = false, done_ = false;
    QSet<QString> modsDown_, windowMods_;
    bool gotMain_ = false, mainUp_ = false;
    KeyCode mainCode_ = 0;
    QString spec_;
};

/* ---------------- pedal card ---------------- */

class PedalCard : public QWidget {
public:
    std::function<void(const QString &)> log;   /* to the main window log */
    QString fpedalBin;

    PedalCard(int n, QWidget *parent = nullptr) : QWidget(parent), n_(n) {
        QGroupBox *box = new QGroupBox(QStringLiteral("Pedal %1").arg(n), this);
        QVBoxLayout *v = new QVBoxLayout(box);

        cur_ = new QLabel("reading device…", box);
        cur_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont f = cur_->font(); f.setBold(true); cur_->setFont(f);
        v->addWidget(cur_);

        QHBoxLayout *h1 = new QHBoxLayout;
        spec_ = new QLineEdit(box);
        spec_->setPlaceholderText("spec, e.g. ctrl+shift+t  ·  l_win  ·  mouse_left  ·  str:hi");
        capBtn_ = new QPushButton("capture combo…", box);
        h1->addWidget(spec_, 1);
        h1->addWidget(capBtn_);
        v->addLayout(h1);

        QHBoxLayout *h2 = new QHBoxLayout;
        h2->addWidget(new QLabel("mouse:", box));
        mL_ = new QCheckBox("left", box);
        mR_ = new QCheckBox("right", box);
        mM_ = new QCheckBox("middle", box);
        h2->addWidget(mL_); h2->addWidget(mR_); h2->addWidget(mM_);
        h2->addSpacing(12);
        h2->addWidget(new QLabel("x:", box));
        sx_ = motionSpin(box);
        h2->addWidget(sx_);
        h2->addWidget(new QLabel("y:", box));
        sy_ = motionSpin(box);
        h2->addWidget(sy_);
        h2->addWidget(new QLabel("w:", box));
        sw_ = motionSpin(box);
        h2->addWidget(sw_);
        addMouse_ = new QPushButton("set mouse in spec", box);
        h2->addWidget(addMouse_);
        h2->addStretch();
        v->addLayout(h2);

        QHBoxLayout *h3 = new QHBoxLayout;
        applyBtn_ = new QPushButton("Apply", box);
        clearBtn_ = new QPushButton("Clear pedal", box);
        status_ = new QLabel(box);
        h3->addWidget(applyBtn_);
        h3->addWidget(clearBtn_);
        h3->addWidget(status_, 1);
        v->addLayout(h3);

        QVBoxLayout *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->addWidget(box);

        connect(capBtn_, &QPushButton::clicked, this, [this] { startCapture(); });
        connect(applyBtn_, &QPushButton::clicked, this, [this] { applySpec(spec_->text().trimmed()); });
        connect(clearBtn_, &QPushButton::clicked, this, [this] { applySpec("none"); });
        connect(addMouse_, &QPushButton::clicked, this, [this] { addMousePart(); });
    }

    QString specText() const { return spec_->text(); }
    QString statusText() const { return status_->text(); }
    void setFpedalBin(const QString &b) { fpedalBin = b; }

    /* drive the exact Apply code path used by the button (used by
     * --selftest-write with a stub fpedal so no device is touched) */
    void applyForTest(const QString &spec) { applySpec(spec); }

    void startCaptureForTest() { startCapture(); }

    void setCurrent(const QJsonObject &ped) {
        cur_->setText("stored on pedal:  " + humanFromJson(ped));
        spec_->setText(specFromJson(ped));
        setStatus(humanFromJson(ped), "#666");
    }

private:
    static QSpinBox *motionSpin(QWidget *parent) {
        QSpinBox *s = new QSpinBox(parent);
        s->setRange(-128, 127);
        s->setFixedWidth(64);
        return s;
    }

    void setStatus(const QString &text, const QString &color) {
        status_->setText(text);
        status_->setStyleSheet("color:" + color + ";");
    }

    QString humanFromJson(const QJsonObject &p) const {
        QString t = p["type"].toString();
        if (t == "none" || t.isEmpty()) return "unconfigured";
        if (t == "string") return QString("types \"%1\"").arg(p["string"].toString());
        QStringList bits;
        for (const QJsonValue &v : p["mods"].toArray())
            bits << modDisplay(v.toString());
        if (!p["key"].toString().isEmpty()) bits << p["key"].toString();
        if (!p["button"].toString().isEmpty())
            bits << "mouse " + p["button"].toString();
        int x = p["x"].toInt(), y = p["y"].toInt(), w = p["w"].toInt();
        if (x || y || w)
            bits << QString("move x:%1 y:%2 w:%3").arg(x).arg(y).arg(w);
        return bits.join(" + ");
    }

    QString specFromJson(const QJsonObject &p) const {
        QString t = p["type"].toString();
        if (t == "none" || t.isEmpty()) return QString();
        if (t == "string") return "str:" + p["string"].toString();
        QStringList mods;
        for (const QJsonValue &v : p["mods"].toArray()) mods << v.toString();
        QString s = mods.join('+');
        if (!p["key"].toString().isEmpty())
            s += (s.isEmpty() ? QString() : "+") + p["key"].toString();
        QString btn = p["button"].toString();
        if (!btn.isEmpty()) {
            if (btn.contains('+')) btn = btn.section('+', 0, 0)
                                    + " (multi-button pedal — edit manually)";
            s += (s.isEmpty() ? QString() : " ") + "mouse_" + btn;
        }
        int x = p["x"].toInt(), y = p["y"].toInt(), w = p["w"].toInt();
        if (x || y || w)
            s += QString(" x:%1 y:%2 w:%3").arg(x).arg(y).arg(w);
        return s;
    }

    /* rebuild the mouse/motion part of the spec from the controls */
    void addMousePart() {
        QString btn;
        if (mL_->isChecked()) btn = "mouse_left";
        else if (mR_->isChecked()) btn = "mouse_right";
        else if (mM_->isChecked()) btn = "mouse_middle";
        QStringList mot;
        if (sx_->value()) mot << QString("x:%1").arg(sx_->value());
        if (sy_->value()) mot << QString("y:%1").arg(sy_->value());
        if (sw_->value()) mot << QString("w:%1").arg(sw_->value());
        QStringList extra;
        if (!btn.isEmpty()) extra << btn;
        extra << mot;
        if (extra.isEmpty()) { setStatus("check a button or set motion first", "#a60"); return; }

        /* drop previous mouse/motion parts, keep the key part */
        QStringList kept;
        for (const QString &tok : spec_->text().split(' ', Qt::SkipEmptyParts)) {
            if (tok.startsWith("mouse_") || tok.startsWith("x:") ||
                tok.startsWith("y:") || tok.startsWith("w:")) continue;
            kept << tok;
        }
        QString s = kept.join(' ');
        if (!s.isEmpty() && !extra.isEmpty()) s += " ";
        spec_->setText(s + QStringList(extra).join(' '));
        setStatus("mouse part updated — press Apply to write", "#06c");
    }

    void startCapture() {
        capBtn_->setText("press combo…");
        capBtn_->setEnabled(false);
        KeyCapture *cap = new KeyCapture(this, status_);
        cap->done = [this, cap](bool ok, const QString &spec, const QString &note) {
            capBtn_->setText("capture combo…");
            capBtn_->setEnabled(true);
            if (ok) {
                spec_->setText(spec);
                if (log) log(QString("pedal %1 captured: %2").arg(n_).arg(spec));
            } else if (log) {
                log(QString("pedal %1 capture: %2").arg(n_).arg(note));
            }
            cap->deleteLater();
        };
        cap->start();
    }

    /* light client-side validation mirroring fpedal's grammar */
    bool specLooksValid(const QString &s, QString *bad) {
        if (s == "none") return true;
        if (s.startsWith("str:")) {
            int len = s.mid(4).length();
            if (len < 1 || len > 38) { *bad = "string must be 1..38 chars"; return false; }
            return true;
        }
        if (s.isEmpty()) { *bad = "spec is empty (Clear pedal to unconfigure)"; return false; }
        static const QSet<QString> mods = {"ctrl","shift","alt","win",
                                           "l_ctrl","l_shift","l_alt","l_win",
                                           "r_ctrl","r_shift","r_alt","r_win"};
        for (const QString &tok : s.split(' ', Qt::SkipEmptyParts)) {
            for (const QString &part : tok.split('+', Qt::SkipEmptyParts)) {
                bool ok = mods.contains(part) || part == "mouse_left" ||
                          part == "mouse_right" || part == "mouse_middle" ||
                          part.startsWith("x:") || part.startsWith("y:") ||
                          part.startsWith("w:") || isValidKeyToken(part);
                if (!ok) { *bad = part; return false; }
            }
        }
        return true;
    }

    void applySpec(const QString &spec) {
        QString bad;
        if (!specLooksValid(spec, &bad)) {
            setStatus("invalid spec: " + bad, "#c00");
            return;
        }
        setEnabled(false);
        setStatus("writing…", "#06c");
        if (log) log(QString("writing pedal %1: %2").arg(n_).arg(spec));

        ProcResult r = runFpedal(fpedalBin,
                                 {"set", QString::number(n_), spec, "--yes", "--json"});
        QJsonObject o = firstJsonLine(r.out);

        if (r.code == 0 && o["ok"].toBool() && o["readback_ok"].toBool()) {
            setCurrent(o["now"].toObject());
            setStatus("written + read-back verified", "#080");
            if (log) log(QString("pedal %1 ok: %2 → %3")
                             .arg(n_).arg(humanFromJson(o["was"].toObject()),
                                          humanFromJson(o["now"].toObject())));
        } else {
            QString why = o["error"].toString();
            if (why.isEmpty()) why = r.err.trimmed().isEmpty() ? "failed" : r.err.trimmed();
            setStatus("FAILED: " + why, "#c00");
            if (log) log(QString("pedal %1 write failed: %2").arg(n_).arg(why));
        }
        setEnabled(true);
    }

    int n_;
    QLabel *cur_, *status_;
    QLineEdit *spec_;
    QPushButton *capBtn_, *applyBtn_, *clearBtn_, *addMouse_;
    QCheckBox *mL_, *mR_, *mM_;
    QSpinBox *sx_, *sy_, *sw_;
};

/* ---------------- main window ---------------- */

class MainWindow : public QWidget {
public:
    MainWindow(bool selftest, bool selftestWrite = false) {
        setWindowTitle("FS3-P foot switch programmer");

        fpedal_ = findFpedal();

        QVBoxLayout *v = new QVBoxLayout(this);

        QHBoxLayout *head = new QHBoxLayout;
        QLabel *title = new QLabel("FS3-P foot switch programmer", this);
        QFont f = title->font(); f.setPointSize(f.pointSize() + 4); f.setBold(true);
        title->setFont(f);
        dev_ = new QLabel(this);
        refreshBtn_ = new QPushButton("refresh", this);
        head->addWidget(title);
        head->addStretch();
        head->addWidget(dev_);
        head->addWidget(refreshBtn_);
        v->addLayout(head);

        for (int i = 0; i < 3; i++) {
            PedalCard *c = new PedalCard(i + 1, this);
            c->fpedalBin = fpedal_;
            c->log = [this](const QString &s) { appendLog(s); };
            cards_.append(c);
            v->addWidget(c);
        }

        QLabel *lt = new QLabel("X events seen by this window "
                                "(focus it and stomp a pedal to test):", this);
        v->addWidget(lt);
        log_ = new QPlainTextEdit(this);
        log_->setReadOnly(true);
        log_->setMaximumBlockCount(400);
        log_->setMinimumHeight(110);
        v->addWidget(log_);

        QLabel *help = new QLabel(
            "specs: ctrl+shift+t · l_win · mouse_left · x:10 y:-5 w:2 · str:hello · none", this);
        help->setStyleSheet("color:#666;");
        v->addWidget(help);

        connect(refreshBtn_, &QPushButton::clicked, this, [this] { refresh(); });

        qApp->installEventFilter(this);

        refresh();
        appendLog(QString("fpedal: %1").arg(fpedal_));
        if (selftest) { qputenv("FPEDAL_UI_VERBOSE", "1"); selfTest(); }
        if (selftestWrite) {
            qputenv("FPEDAL_UI_VERBOSE", "1");
            QTimer::singleShot(0, this, [this]{ selfTestWrite(); });
        }
    }

protected:
    bool eventFilter(QObject *o, QEvent *e) override {
        QString line;
        switch (e->type()) {
        case QEvent::KeyPress: {
            auto *ke = static_cast<QKeyEvent *>(e);
            line = "key press    " + qtKeyText(ke);
            break; }
        case QEvent::KeyRelease: {
            auto *ke = static_cast<QKeyEvent *>(e);
            line = "key release  " + qtKeyText(ke);
            break; }
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(e);
            line = QString("button press %1").arg(me->button());
            break; }
        case QEvent::MouseButtonRelease: {
            auto *me = static_cast<QMouseEvent *>(e);
            line = QString("button release %1").arg(me->button());
            break; }
        default:
            break;
        }
        if (!line.isEmpty()) appendLog(line);
        return QWidget::eventFilter(o, e);
    }

private:
    static QString qtKeyText(QKeyEvent *ke) {
        QString s = QKeySequence(ke->key()).toString(QKeySequence::NativeText);
        QStringList mods;
        if (ke->modifiers() & Qt::ControlModifier) mods << "Ctrl";
        if (ke->modifiers() & Qt::ShiftModifier)   mods << "Shift";
        if (ke->modifiers() & Qt::AltModifier)     mods << "Alt";
        if (ke->modifiers() & Qt::MetaModifier)    mods << "Win";
        if (!mods.isEmpty()) s = mods.join('+') + "+" + s;
        return s;
    }

    static QString findFpedal() {
        QString here = QCoreApplication::applicationDirPath();
        const QStringList candidates = {
            here + "/fpedal",
            here + "/../fpedal",
            QDir::currentPath() + "/fpedal",
            "fpedal",
        };
        for (const QString &p : candidates)
            if (p == "fpedal" || QFile::exists(p)) return p;
        return "fpedal";
    }

    void refresh() {
        ProcResult r = runFpedal(fpedal_, {"show", "--json"});
        QJsonObject o = firstJsonLine(r.out);
        if (r.code != 0 || o.isEmpty()) {
            dev_->setText("no pedal");
            dev_->setStyleSheet("color:#c00;");
            appendLog("state read failed: " +
                      (r.err.trimmed().isEmpty() ? o["error"].toString() : r.err.trimmed()));
            return;
        }
        dev_->setStyleSheet("");
        dev_->setText(QString("device %1:%2").arg(o["vid"].toString(), o["pid"].toString()));
        const QJsonArray pedals = o["pedals"].toArray();
        for (int i = 0; i < 3 && i < pedals.size(); i++)
            cards_[i]->setCurrent(pedals[i].toObject());
        appendLog("read pedal state ok");
    }

    void appendLog(const QString &s) {
        log_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss") + "  " + s);
        if (getenv("FPEDAL_UI_VERBOSE"))
            fprintf(stderr, "%s\n", s.toUtf8().constData());
    }

    /* drive the real Apply code path (arg build -> QProcess -> JSON parse
     * -> card + status update) against a stub fpedal so no device is
     * touched. Exit code 0 = pass. */
    void selfTestWrite() {
        QString stub = QDir::tempPath() + "/fpedal-stub-" +
                       QString::number(QCoreApplication::applicationPid()) + ".sh";
        QFile f(stub);
        if (!f.open(QIODevice::WriteOnly)) {
            appendLog("selftest-write: cannot create stub"); qApp->exit(2); return;
        }
        f.write(
            "#!/bin/sh\n"
            "printf '%s\\n' '{\"ok\":true,\"pedal\":1,"
            "\"was\":{\"n\":1,\"x\":0,\"y\":0,\"w\":0,\"type\":\"none\","
            "\"mods\":[],\"key\":null,\"button\":null,\"string\":null},"
            "\"now\":{\"n\":1,\"x\":0,\"y\":0,\"w\":0,\"type\":\"key\","
            "\"mods\":[\"l_ctrl\"],\"key\":\"a\",\"button\":null,\"string\":null},"
            "\"readback_ok\":true}'\nexit 0\n");
        f.close();
        QFile::setPermissions(stub, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                        QFileDevice::ExeOwner);

        cards_[0]->setFpedalBin(stub);
        cards_[0]->applyForTest("l_ctrl+a");
        QString spec = cards_[0]->specText().trimmed();
        QString status = cards_[0]->statusText();
        bool pass = (spec == "l_ctrl+a") && status.contains("verified");
        appendLog(QString("selftest-write: pedal1 spec='%1' status='%2' -> %3")
                      .arg(spec, status, pass ? "PASS" : "FAIL"));
        QFile::remove(stub);
        qApp->exit(pass ? 0 : 1);
    }

    /* selftest: arm capture on pedal 1, inject ctrl+shift+t with XTEST,
     * verify the captured spec. Used by the test target in the Makefile. */
    void selfTest() {
        appendLog("selftest: arming capture on pedal 1");
        QTimer::singleShot(1200, this, [this] {
            cards_[0]->startCaptureForTest();
            QTimer::singleShot(1200, this, [this] {
                Display *d = XOpenDisplay(nullptr);
                if (!d) { appendLog("selftest: no display for XTEST"); qApp->exit(2); return; }
                KeyCode kc = XKeysymToKeycode(d, XK_Control_L);
                KeyCode ks = XKeysymToKeycode(d, XK_Shift_L);
                KeyCode kt = XKeysymToKeycode(d, XK_t);
                XTestFakeKeyEvent(d, kc, True, 0);
                XTestFakeKeyEvent(d, ks, True, 0);
                XTestFakeKeyEvent(d, kt, True, 0);
                XTestFakeKeyEvent(d, kt, False, 0);
                XTestFakeKeyEvent(d, ks, False, 0);
                XTestFakeKeyEvent(d, kc, False, 0);
                XSync(d, False);
                XCloseDisplay(d);
                appendLog("selftest: injected ctrl+shift+t");
            });
            QTimer::singleShot(6000, this, [this] {
                QString got = cards_[0]->specText().trimmed();
                bool pass = (got == "l_ctrl+l_shift+t");
                appendLog(QString("selftest: pedal 1 spec '%1' -> %2")
                              .arg(got, pass ? "PASS" : "FAIL"));
                qApp->exit(pass ? 0 : 1);
            });
        });
    }

    QString fpedal_;
    QLabel *dev_;
    QPushButton *refreshBtn_;
    QList<PedalCard *> cards_;
    QPlainTextEdit *log_;
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("fpedal-ui");
    bool selftest = false, selftestWrite = false;
    for (int i = 1; i < argc; i++) {
        QString a(argv[i]);
        if (a == "--selftest") selftest = true;
        else if (a == "--selftest-write") selftestWrite = true;
    }
    MainWindow w(selftest, selftestWrite);
    w.resize(780, 760);
    w.show();
    return app.exec();
}
