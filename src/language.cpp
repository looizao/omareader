#include "language.h"

#include "book.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <algorithm>

namespace {

struct Known {
    const char *code;
    const char *name;
    // Other ways books and settings name it: ISO 639-2 codes and names in
    // English and in the language itself, all lower case.
    QStringList aliases;
};

const QList<Known> &known() {
    static const QList<Known> list = {
        {"en", "English", {"eng", "english", "inglês", "ingles"}},
        {"pt", "Portuguese", {"por", "portuguese", "português", "portugues"}},
        {"es", "Spanish", {"spa", "spanish", "español", "espanol", "castellano"}},
        {"fr", "French", {"fra", "fre", "french", "français", "francais"}},
        {"it", "Italian", {"ita", "italian", "italiano"}},
        {"de", "German", {"deu", "ger", "german", "deutsch"}},
        {"nl", "Dutch", {"nld", "dut", "dutch", "nederlands"}},
        {"ru", "Russian", {"rus", "russian", "русский"}},
        {"uk", "Ukrainian", {"ukr", "ukrainian", "українська"}},
        {"pl", "Polish", {"pol", "polish", "polski"}},
        {"sv", "Swedish", {"swe", "swedish", "svenska"}},
        {"tr", "Turkish", {"tur", "turkish", "türkçe"}},
        {"el", "Greek", {"ell", "gre", "greek", "ελληνικά"}},
        {"ar", "Arabic", {"ara", "arabic", "العربية"}},
        {"he", "Hebrew", {"heb", "hebrew", "iw", "עברית"}},
        {"hi", "Hindi", {"hin", "hindi", "हिन्दी"}},
        {"th", "Thai", {"tha", "thai", "ไทย"}},
        {"zh", "Chinese", {"zho", "chi", "cmn", "yue", "chinese", "mandarin", "中文"}},
        {"ja", "Japanese", {"jpn", "japanese", "日本語"}},
        {"ko", "Korean", {"kor", "korean", "한국어"}},
    };
    return list;
}

// The commonest little words of each language written in Latin letters.
// Several turn up in more than one language ("de", "la", "in"); those count
// for each, and the words only one language has decide between them.
const QList<std::pair<QString, QSet<QString>>> &functionWords() {
    static const QList<std::pair<QString, QSet<QString>>> lists = [] {
        const QList<std::pair<const char *, const char *>> raw = {
            {"en", "the and of to was that with for it he she his her you are be have had this which from not they "
                   "were been would will there their at by but or what when who"},
            {"pt", "o os as e do da dos das em no na nos nas um uma com não para mais ao é foi ele ela mas seu sua "
                   "também quando muito isso você está pelo pela então já há sobre entre"},
            {"es", "el los las y del en por con una es lo su al más pero sus le ya muy también cuando está fue este "
                   "esta sin sobre hay yo él ella todo porque"},
            {"fr", "le la les et des du un une est que qui dans pour pas sur au avec il elle ce se ne sont mais ont "
                   "été aux cette nous vous leur je plus comme"},
            {"it", "il lo gli le di e che della del delle dei in un una per non con è sono si al alla anche ma come "
                   "più questo questa nel nella ha era loro"},
            {"de", "der die das und ist nicht ein eine zu den dem des mit sich auf für von auch es im sie er wir "
                   "ich war wird werden aber oder wenn noch nach wie aus bei nur über"},
            {"nl", "de het een en van is dat op te zijn niet met voor die er aan hij ze maar om ook als bij nog naar "
                   "wordt worden uit door wat kan heeft haar dit deze wij zij geen"},
        };
        QList<std::pair<QString, QSet<QString>>> out;
        for (const auto &[code, words] : raw) {
            const QStringList list = QString::fromUtf8(words).split(' ', Qt::SkipEmptyParts);
            out.append({code, QSet<QString>(list.begin(), list.end())});
        }
        return out;
    }();
    return lists;
}

Language::Guess guessLatin(const QString &text) {
    static const QRegularExpression word(R"(\p{L}+)");
    QHash<QString, int> scores;
    int words = 0;
    for (auto it = word.globalMatch(text); it.hasNext();) {
        const QString w = it.next().captured().toLower();
        ++words;
        for (const auto &[code, list] : functionWords())
            if (list.contains(w))
                ++scores[code];
    }
    QString best;
    int top = 0, second = 0;
    for (auto it = scores.cbegin(); it != scores.cend(); ++it) {
        if (it.value() > top) {
            second = top;
            top = it.value();
            best = it.key();
        } else if (it.value() > second) {
            second = it.value();
        }
    }
    // Too few words to tell, or none of these languages: in a real text,
    // little words are one in every four or five.
    if (top < 8 || top < words * 0.08)
        return {};
    // A close race (Spanish against Portuguese, say) says nothing either.
    const double lead = 1.0 - double(second) / top;
    if (lead < 0.25)
        return {};
    // Short texts can't be very sure, however clear they look.
    const double size = std::min(1.0, words / 200.0);
    return {best, std::min(1.0, lead * 1.5) * size};
}

}

QString Language::fromTag(const QString &tag) {
    const QString t = tag.trimmed().toLower().replace('_', '-');
    if (t.isEmpty())
        return {};
    const QString primary = t.section('-', 0, 0);
    for (const Known &k : known()) {
        if (primary == QLatin1String(k.code) || k.aliases.contains(t) || k.aliases.contains(primary))
            return k.code;
    }
    return {};
}

QString Language::name(const QString &code) {
    for (const Known &k : known())
        if (code == QLatin1String(k.code))
            return k.name;
    return {};
}

Language::Guess Language::guess(const QString &text) {
    QHash<int, int> scripts;
    int letters = 0, kana = 0, ukrainian = 0;
    for (const QChar c : text) {
        if (!c.isLetter())
            continue;
        ++letters;
        const QChar::Script script = c.script();
        ++scripts[script];
        if (script == QChar::Script_Hiragana || script == QChar::Script_Katakana)
            ++kana;
        // Letters Ukrainian has and Russian doesn't.
        if (QStringLiteral("іїєґІЇЄҐ").contains(c))
            ++ukrainian;
    }
    if (letters < 20)
        return {};
    // Han characters stand for many letters' worth of text each, so a page
    // of Chinese or Japanese is clear long before it has many of them.
    const int cjk = scripts.value(QChar::Script_Han) + kana;
    const double cjkShare = double(cjk) / letters;
    if (cjkShare > 0.5) {
        if (double(kana) / cjk > 0.1)
            return {"ja", cjkShare};
        return {"zh", cjkShare};
    }
    const QList<std::pair<QChar::Script, const char *>> others = {
        {QChar::Script_Hangul, "ko"}, {QChar::Script_Cyrillic, "ru"}, {QChar::Script_Devanagari, "hi"},
        {QChar::Script_Greek, "el"},  {QChar::Script_Arabic, "ar"},   {QChar::Script_Hebrew, "he"},
        {QChar::Script_Thai, "th"},
    };
    for (const auto &[script, code] : others) {
        const double share = double(scripts.value(script)) / letters;
        if (share <= 0.5)
            continue;
        if (script == QChar::Script_Cyrillic && ukrainian > scripts.value(script) / 100)
            return {"uk", share};
        return {code, share};
    }
    if (double(scripts.value(QChar::Script_Latin)) / letters > 0.5)
        return guessLatin(text);
    return {};
}

QString Language::sample(const Book &book, int maxWords) {
    // Short stretches from all through the book, so no one chapter (a
    // foreword, a glossary, a licence) speaks for all of it.
    const int count = book.sentenceCount();
    if (count == 0)
        return {};
    constexpr int Stretch = 8; // sentences in a row
    const int stretches = std::max(1, std::min(count / Stretch, maxWords / 120));
    QStringList parts;
    int words = 0;
    for (int i = 0; i < stretches && words < maxWords; ++i) {
        const int first = int(qint64(count) * i / stretches);
        for (int s = first; s < std::min(count, first + Stretch) && words < maxWords; ++s) {
            const Sentence &sentence = book.sentences.at(s);
            if (book.chapters.at(sentence.chapter).paragraphs.at(sentence.paragraph).note)
                continue;
            parts << book.sentenceText(s);
            words += int(sentence.words.size());
        }
    }
    return parts.join(' ');
}

void Language::identify(Book &book) {
    const Guess text = guess(sample(book));
    if (!book.language.isEmpty() && (text.code.isEmpty() || text.code == book.language
                                     || text.confidence < StrongGuess)) {
        book.languageSource = "metadata";
        return;
    }
    book.language = text.code;
    book.languageSource = text.code.isEmpty() ? QString() : QStringLiteral("text");
}
