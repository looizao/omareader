#include "voicesettings.h"

#include "kokoro.h"

#include <QSettings>
#include <cmath>

namespace {

// QSettings keys. Kokoro's voice and speed keep the names they had before
// there was a second engine, so upgrading keeps the user's choices.
QString voiceKey(const QString &engine) { return engine == Engine::Kokoro ? "voice/name" : "voice/" + engine + "/voice"; }
QString speedKey(const QString &engine) { return engine == Engine::Kokoro ? "voice/speed" : "voice/" + engine + "/speed"; }
QString languageKey(const QString &engine) { return "voice/" + engine + "/language"; }
QString tuningKey(const QString &engine, const QString &key) { return "voice/" + engine + "/tuning/" + key; }

TuningParameter number(const QString &key, const QString &label, const QString &description, double minimum,
                       double maximum, double step, double recommended, const QString &unit = {}) {
    TuningParameter p;
    p.key = key;
    p.kind = TuningParameter::Number;
    p.label = label;
    p.description = description;
    p.minimum = minimum;
    p.maximum = maximum;
    p.step = step;
    p.recommended = recommended;
    p.unit = unit;
    return p;
}

TuningParameter choice(const QString &key, const QString &label, const QString &description,
                       const QStringList &choices, const QStringList &labels, const QString &recommended) {
    TuningParameter p;
    p.key = key;
    p.kind = TuningParameter::Choice;
    p.label = label;
    p.description = description;
    p.choices = choices;
    p.choiceLabels = labels;
    p.recommended = recommended;
    return p;
}

// Both engines get these; each engine still keeps its own values, since
// the voices differ in loudness and pacing.
QList<TuningParameter> sharedParameters() {
    return {
        number("volume", "Volume",
               "Louder or softer than the voice's own level. Past 100% the loudest syllables may be clipped.", 50,
               200, 5, 100, "%"),
        number("paragraphPause", "Pause after a paragraph",
               "Silence after each paragraph, so you hear where one ends and the next begins.", 0, 2, 0.05, 0.45, "s"),
        number("headingPause", "Pause after a heading", "Silence after a chapter title or a heading.", 0, 3, 0.05,
               0.9, "s"),
    };
}

}

QStringList Engine::all() { return {Kokoro, Qwen}; }
bool Engine::isKnown(const QString &engine) { return all().contains(engine); }
QString Engine::displayName(const QString &engine) { return engine == Qwen ? "Qwen3-TTS" : "Kokoro"; }

QList<EngineVoice> EngineCatalog::voices(const QString &engine) {
    QList<EngineVoice> out;
    if (engine == Engine::Qwen) {
        // The nine speakers of Qwen3-TTS's CustomVoice models. Each can read
        // every language the model supports, best in its own.
        static const QList<std::array<const char *, 4>> speakers = {
            {"ryan", "Ryan", "English", "Dynamic male voice with strong rhythmic drive"},
            {"aiden", "Aiden", "English", "Sunny American male voice with a clear midrange"},
            {"vivian", "Vivian", "Chinese", "Bright, slightly edgy young female voice"},
            {"serena", "Serena", "Chinese", "Warm, gentle young female voice"},
            {"uncle_fu", "Uncle Fu", "Chinese", "Seasoned male voice with a low, mellow timbre"},
            {"dylan", "Dylan", "Chinese", "Youthful Beijing male voice with a clear, natural timbre"},
            {"eric", "Eric", "Chinese", "Lively Chengdu male voice with a slightly husky brightness"},
            {"ono_anna", "Ono Anna", "Japanese", "Playful Japanese female voice with a light, nimble timbre"},
            {"sohee", "Sohee", "Korean", "Warm Korean female voice with rich emotion"},
        };
        for (const auto &s : speakers)
            out.append({s[0], s[1], s[2], s[3]});
        return out;
    }
    for (const Voice &v : Voices::all())
        out.append({v.id, v.name, v.language, {}});
    return out;
}

QList<EngineLanguage> EngineCatalog::languages(const QString &engine) {
    if (engine != Engine::Qwen)
        return {};
    return {{"English", "English"},   {"Portuguese", "Portuguese"}, {"Spanish", "Spanish"},
            {"French", "French"},     {"German", "German"},         {"Italian", "Italian"},
            {"Russian", "Russian"},   {"Chinese", "Chinese"},       {"Japanese", "Japanese"},
            {"Korean", "Korean"},     {"Auto", "Detect from the text"}};
}

bool EngineCatalog::hasVoice(const QString &engine, const QString &voice) {
    for (const EngineVoice &v : voices(engine))
        if (v.id == voice)
            return true;
    return false;
}

bool EngineCatalog::hasLanguage(const QString &engine, const QString &language) {
    for (const EngineLanguage &l : languages(engine))
        if (l.id == language)
            return true;
    return false;
}

QString EngineCatalog::previewLanguage(const QString &engine, const QString &voice, const QString &language) {
    static const QHash<QString, QString> codes = {
        {"english", "en"}, {"portuguese", "pt"}, {"spanish", "es"}, {"french", "fr"},  {"german", "de"},
        {"italian", "it"}, {"russian", "ru"},    {"chinese", "zh"}, {"japanese", "ja"}, {"korean", "ko"},
    };
    if (engine == Engine::Qwen) {
        QString name = language.toLower();
        if (name.isEmpty() || name == "auto") {
            for (const EngineVoice &v : voices(engine))
                if (v.id == voice)
                    name = v.language.toLower();
        }
        return codes.value(name, "en");
    }
    const QString code = Voices::languageFor(voice);
    if (code == "cmn")
        return "zh";
    return code.left(2);
}

QString EngineCatalog::previewText(const QString &languageCode) {
    // The same short story opening in each language, with a comma and a
    // question so the preview shows phrasing as well as the voice.
    static const QHash<QString, QString> texts = {
        {"en", "Once upon a time, in a quiet town by the sea, a girl found a book that could read itself aloud. "
               "Would you like to hear it?"},
        {"pt", "Era uma vez, numa cidade tranquila à beira-mar, uma menina que encontrou um livro capaz de ler a si "
               "mesmo em voz alta. Você quer ouvir?"},
        {"es", "Había una vez, en un pueblo tranquilo junto al mar, una niña que encontró un libro que sabía leerse "
               "en voz alta. ¿Quieres oírlo?"},
        {"fr", "Il était une fois, dans une petite ville tranquille au bord de la mer, une fillette qui trouva un "
               "livre capable de se lire à voix haute. Veux-tu l'entendre ?"},
        {"it", "C'era una volta, in un paese tranquillo vicino al mare, una bambina che trovò un libro capace di "
               "leggersi ad alta voce. Vuoi sentirlo?"},
        {"de", "Es war einmal, in einer ruhigen Stadt am Meer, ein Mädchen, das ein Buch fand, das sich selbst "
               "vorlesen konnte. Möchtest du es hören?"},
        {"ru", "Жила-была в тихом городке у моря девочка, которая нашла книгу, умевшую читать себя вслух. Хочешь "
               "послушать?"},
        {"zh", "从前，在海边一个安静的小镇上，一个女孩发现了一本会自己朗读的书。你想听听吗？"},
        {"ja", "昔々、海辺の静かな町で、一人の少女が自分で声に出して読む本を見つけました。聞いてみますか？"},
        {"ko", "옛날 옛적, 바닷가의 조용한 마을에서 한 소녀가 스스로 소리 내어 읽는 책을 발견했습니다. 들어 볼래요?"},
        {"hi", "बहुत समय पहले, समुद्र के किनारे एक शांत शहर में, एक लड़की को एक ऐसी किताब मिली जो खुद को पढ़कर सुना "
               "सकती थी। क्या आप इसे सुनना चाहेंगे?"},
    };
    return texts.value(languageCode, texts.value("en"));
}

QList<TuningParameter> Tuning::parameters(const QString &engine) {
    QList<TuningParameter> out;
    if (engine == Engine::Qwen) {
        out.append(choice("model", "Model",
                          "The 1.7B model sounds richer and follows a narration style, but needs a GPU with about "
                          "7 GB free. Install it with: omareader setup --engine qwen --model 1.7b",
                          {"0.6b", "1.7b"}, {"0.6B, fits a 6 GB GPU", "1.7B, needs about 7 GB"}, "0.6b"));
        out.append(choice("device", "Runs on",
                          "On the CPU, Qwen3-TTS reads about three times slower than real time: fine for an "
                          "export left running, too slow to listen along.",
                          {"auto", "cuda", "cpu"}, {"The GPU if there is one", "The GPU (CUDA)", "The CPU"}, "auto"));
        TuningParameter style;
        style.key = "style";
        style.kind = TuningParameter::Text;
        style.label = "Narration style";
        style.description = "How to read, in a few words, such as: calm, warm audiobook narration. Only the 1.7B "
                            "model follows it.";
        style.recommended = QString();
        style.dependsOn = "model=1.7b";
        out.append(style);
        // 0.9 is Qwen's own default. Lower made no reliable difference to how
        // often words were dropped or misread in English and Portuguese
        // prose and dialogue, and flattened the delivery.
        out.append(number("temperature", "Expressiveness",
                          "Higher varies the intonation more; lower is steadier and less likely to stumble.", 0.3,
                          1.2, 0.05, 0.9));
        out.append(number("topP", "Variety",
                          "Lower keeps the model to its likeliest sounds, for a more even delivery.", 0.5, 1.0, 0.05,
                          1.0));
        out.append(number("repetitionPenalty", "Repetition guard",
                          "Discourages repeated sounds. Too high can clip or rush words.", 1.0, 1.3, 0.01, 1.05));
        out.append(number("take", "Take",
                          "Each take is a different reading of the same text. Try another if a voice sounds off.", 1,
                          99, 1, 1));
    } else {
        TuningParameter blend = choice("blend", "Blend with",
                                       "Mixes a second voice into the first, for a voice of your own.", {""},
                                       {"No second voice"}, "");
        for (const Voice &v : Voices::all()) {
            blend.choices.append(v.id);
            blend.choiceLabels.append(QString("%1 (%2)").arg(v.name, v.id));
        }
        out.append(blend);
        TuningParameter amount = number("blendAmount", "Blend amount", "How much of the second voice is in the mix.",
                                        10, 90, 10, 30, "%");
        amount.dependsOn = "blend!=";
        out.append(amount);
    }
    out.append(sharedParameters());
    return out;
}

const TuningParameter *Tuning::find(const QList<TuningParameter> &parameters, const QString &key) {
    for (const TuningParameter &p : parameters)
        if (p.key == key)
            return &p;
    return nullptr;
}

QVariant Tuning::normalize(const TuningParameter &parameter, const QVariant &value, bool *ok) {
    if (ok)
        *ok = true;
    switch (parameter.kind) {
    case TuningParameter::Number: {
        bool valid = false;
        double number = value.toDouble(&valid);
        if (!valid || !std::isfinite(number)) {
            if (ok)
                *ok = false;
            return parameter.recommended;
        }
        number = qBound(parameter.minimum, number, parameter.maximum);
        if (parameter.step > 0)
            number = parameter.minimum + std::round((number - parameter.minimum) / parameter.step) * parameter.step;
        // Rounded so 0.45 is stored and shown as 0.45, not 0.44999999.
        return std::round(qBound(parameter.minimum, number, parameter.maximum) * 1000) / 1000;
    }
    case TuningParameter::Choice: {
        const QString text = value.toString();
        if (!parameter.choices.contains(text)) {
            if (ok)
                *ok = false;
            return parameter.recommended;
        }
        return text;
    }
    case TuningParameter::Text:
        return value.toString().simplified().left(300);
    }
    return parameter.recommended;
}

double VoiceSettings::number(const QString &key) const {
    if (tuning.contains(key))
        return tuning.value(key).toDouble();
    const QList<TuningParameter> parameters = Tuning::parameters(engine);
    const TuningParameter *p = Tuning::find(parameters, key);
    return p ? p->recommended.toDouble() : 0;
}

QString VoiceSettings::text(const QString &key) const {
    if (tuning.contains(key))
        return tuning.value(key).toString();
    const QList<TuningParameter> parameters = Tuning::parameters(engine);
    const TuningParameter *p = Tuning::find(parameters, key);
    return p ? p->recommended.toString() : QString();
}

double VoiceSettings::clampSpeed(double speed) {
    if (!std::isfinite(speed))
        speed = 1.0;
    return qBound(MinimumSpeed, std::round(speed * 20) / 20, MaximumSpeed);
}

bool VoiceSettings::set(const QString &key, const QVariant &value, QString *error) {
    auto failed = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (key == "voice") {
        if (!EngineCatalog::hasVoice(engine, value.toString()))
            return failed(QString("%1 has no voice %2.").arg(Engine::displayName(engine), value.toString()));
        voice = value.toString();
        return true;
    }
    if (key == "language") {
        if (!EngineCatalog::hasLanguage(engine, value.toString()))
            return failed(QString("%1 does not read %2.").arg(Engine::displayName(engine), value.toString()));
        language = value.toString();
        return true;
    }
    if (key == "speed") {
        bool ok = false;
        const double s = value.toDouble(&ok);
        if (!ok || s < MinimumSpeed || s > MaximumSpeed)
            return failed("The speed goes from 0.5 to 2.0.");
        speed = clampSpeed(s);
        return true;
    }
    const QList<TuningParameter> parameters = Tuning::parameters(engine);
    const TuningParameter *p = Tuning::find(parameters, key);
    if (!p)
        return failed(QString("%1 has no setting %2.").arg(Engine::displayName(engine), key));
    bool ok = false;
    const QVariant normalized = Tuning::normalize(*p, value, &ok);
    if (!ok) {
        if (p->kind == TuningParameter::Choice)
            return failed(QString("%1 is one of: %2.").arg(key, p->choices.join(", ")));
        return failed(QString("%1 is a number from %2 to %3.").arg(key).arg(p->minimum).arg(p->maximum));
    }
    if (p->kind == TuningParameter::Number) {
        const double requested = value.toDouble();
        if (requested < p->minimum - 1e-9 || requested > p->maximum + 1e-9)
            return failed(QString("%1 is a number from %2 to %3.").arg(key).arg(p->minimum).arg(p->maximum));
    }
    tuning.insert(key, normalized);
    return true;
}

QVariantMap VoiceSettings::toMap() const {
    QVariantMap map{{"engine", engine}, {"voice", voice}, {"speed", speed}, {"tuning", tuning}};
    if (engine == Engine::Qwen)
        map.insert("language", language);
    return map;
}

VoiceSettings VoiceSettings::recommended(const QString &engine) {
    VoiceSettings settings;
    settings.engine = Engine::isKnown(engine) ? engine : Engine::Kokoro;
    if (settings.engine == Engine::Qwen) {
        settings.voice = "ryan";
        settings.language = "English";
    } else {
        settings.voice = "af_heart";
    }
    settings.speed = 1.0;
    for (const TuningParameter &p : Tuning::parameters(settings.engine))
        settings.tuning.insert(p.key, p.recommended);
    return settings;
}

VoiceSettings VoiceSettings::load(const QString &engine) {
    VoiceSettings settings = recommended(engine);
    QSettings stored;
    const QString voice = stored.value(voiceKey(settings.engine)).toString();
    if (EngineCatalog::hasVoice(settings.engine, voice))
        settings.voice = voice;
    if (stored.contains(speedKey(settings.engine)))
        settings.speed = clampSpeed(stored.value(speedKey(settings.engine)).toDouble());
    if (settings.engine == Engine::Qwen) {
        const QString language = stored.value(languageKey(settings.engine)).toString();
        if (EngineCatalog::hasLanguage(settings.engine, language))
            settings.language = language;
    }
    for (const TuningParameter &p : Tuning::parameters(settings.engine)) {
        const QString key = tuningKey(settings.engine, p.key);
        if (!stored.contains(key))
            continue;
        bool ok = false;
        const QVariant value = Tuning::normalize(p, stored.value(key), &ok);
        if (ok)
            settings.tuning.insert(p.key, value);
    }
    return settings;
}

void VoiceSettings::save() const {
    QSettings stored;
    stored.setValue(voiceKey(engine), voice);
    stored.setValue(speedKey(engine), speed);
    if (engine == Engine::Qwen)
        stored.setValue(languageKey(engine), language);
    // Only what differs from the recommendation is stored, so a better
    // recommendation in a later version reaches everyone who never changed it.
    for (const TuningParameter &p : Tuning::parameters(engine)) {
        const QString key = tuningKey(engine, p.key);
        const QVariant value = tuning.value(p.key, p.recommended);
        if (value == p.recommended || (p.kind == TuningParameter::Number
                                       && qFuzzyCompare(1 + value.toDouble(), 1 + p.recommended.toDouble())))
            stored.remove(key);
        else
            stored.setValue(key, value);
    }
}

void VoiceSettings::reset(const QString &engine) {
    QSettings stored;
    stored.remove(speedKey(engine));
    stored.remove("voice/" + engine + "/tuning");
}

QString VoiceSettings::selectedEngine() {
    const QString engine = QSettings().value("voice/engine", Engine::Kokoro).toString();
    return Engine::isKnown(engine) ? engine : Engine::Kokoro;
}

void VoiceSettings::setSelectedEngine(const QString &engine) {
    if (Engine::isKnown(engine))
        QSettings().setValue("voice/engine", engine);
}
