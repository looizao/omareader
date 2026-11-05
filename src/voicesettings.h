#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

// The two voices omareader can read with. Kokoro (through Kokoros) is the
// default and always available once set up; Qwen3-TTS is optional and only
// installed by `omareader setup --engine qwen`.
namespace Engine {
inline const QString Kokoro = QStringLiteral("kokoro");
inline const QString Qwen = QStringLiteral("qwen");
QStringList all();
bool isKnown(const QString &engine);
// "Kokoro", "Qwen3-TTS".
QString displayName(const QString &engine);
}

// A voice the engine offers, and the language it is native to.
struct EngineVoice {
    QString id;
    QString name;
    QString language; // Kokoro: the espeak-ng code; Qwen: the language it was trained on
    QString description;
};

// A language the engine reads in. Kokoro takes its language from the voice,
// so it has none of these.
struct EngineLanguage {
    QString id;   // what the engine is given, such as "English"
    QString name; // untranslated, for qsTr() in the window
};

namespace EngineCatalog {
QList<EngineVoice> voices(const QString &engine);
QList<EngineLanguage> languages(const QString &engine);
bool hasVoice(const QString &engine, const QString &voice);
bool hasLanguage(const QString &engine, const QString &language);
// The language a short preview sentence is in, as a lowercase ISO code.
QString previewLanguage(const QString &engine, const QString &voice, const QString &language);
// A sentence or two to hear a voice by, in the language it reads.
QString previewText(const QString &languageCode);
}

// One adjustable setting in the voice panel's Advanced section. Labels and
// descriptions stay in English here; the window and the CLI translate them.
struct TuningParameter {
    enum Kind { Number, Text, Choice };
    QString key;
    Kind kind = Number;
    QString label;
    QString description;
    double minimum = 0;
    double maximum = 1;
    double step = 0.1;
    QString unit; // "s", "%" or empty
    QVariant recommended;
    QStringList choices;      // Choice: the stored values
    QStringList choiceLabels; // Choice: what the window shows for each
    // Only shown and used when the engine's model supports it, such as Qwen's
    // style instructions, which the 0.6B model ignores.
    QString dependsOn;
};

namespace Tuning {
QList<TuningParameter> parameters(const QString &engine);
const TuningParameter *find(const QList<TuningParameter> &parameters, const QString &key);
// value made valid for the parameter: numbers clamped and rounded to its
// step, choices checked, text trimmed. ok is false when it can't be.
QVariant normalize(const TuningParameter &parameter, const QVariant &value, bool *ok = nullptr);
}

// Everything that decides how a voice sounds: the engine, its voice and
// language, the speed and the tuning. A reading, a preview and an export
// built from the same VoiceSettings sound the same, and an export keeps the
// copy it started with even when the window's settings change.
struct VoiceSettings {
    QString engine = Engine::Kokoro;
    QString voice;
    QString language; // Qwen only
    double speed = 1.0;
    QVariantMap tuning; // every parameter of the engine, with its effective value

    double number(const QString &key) const;
    QString text(const QString &key) const;
    // Applies one "key=value" style override; false with a reason if the key
    // or value isn't valid for this engine.
    bool set(const QString &key, const QVariant &value, QString *error = nullptr);
    QVariantMap toMap() const;

    // The settings omareader recommends for the engine, used for anything the
    // user hasn't chosen and by the reset button.
    static VoiceSettings recommended(const QString &engine);
    // The user's saved settings for the engine; each engine keeps its own.
    static VoiceSettings load(const QString &engine);
    static VoiceSettings loadSelected() { return load(selectedEngine()); }
    void save() const;
    // Forgets the speed and tuning saved for the engine, keeping its voice.
    static void reset(const QString &engine);

    static QString selectedEngine();
    static void setSelectedEngine(const QString &engine);

    static constexpr double MinimumSpeed = 0.5;
    static constexpr double MaximumSpeed = 2.0;
    static double clampSpeed(double speed);
};
