#include "pronunciation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

namespace {

bool hasLetterOrDigit(const QString &text) {
    return std::any_of(text.begin(), text.end(), [](QChar c) { return c.isLetterOrNumber(); });
}

// Written in capitals: at least two letters, none of them lower case.
bool capitals(const QString &written) {
    int letters = 0;
    for (const QChar c : written) {
        if (!c.isLetter())
            continue;
        if (c.isLower())
            return false;
        letters += c.isUpper();
    }
    return letters >= 2;
}

// One entry as a pattern: its words with any run of spaces between them,
// and either apostrophe for an apostrophe, since books curl them.
QString patternFor(const QString &written) {
    QStringList words;
    for (const QString &word : written.split(' ', Qt::SkipEmptyParts)) {
        QString piece;
        for (const QChar c : word)
            piece += c == '\'' || c == QChar(0x2019) ? QStringLiteral("['’]") : QRegularExpression::escape(QString(c));
        words.append(piece);
    }
    return words.join(QStringLiteral("\\s+"));
}

}

QString Lexicon::defaultFile() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/pronunciations.json";
}

QString Lexicon::key(const QString &written) {
    const QString simple = written.simplified();
    return capitals(simple) ? simple : simple.toCaseFolded();
}

bool Lexicon::valid(const QString &written, const QString &spoken) {
    const QString w = written.simplified();
    return hasLetterOrDigit(w) && hasLetterOrDigit(spoken) && w.count(' ') < 6 && w.size() <= 80
           && spoken.simplified().size() <= 200;
}

Lexicon Lexicon::load(const QString &file) {
    Lexicon lexicon;
    QFile in(file.isEmpty() ? defaultFile() : file);
    if (!in.open(QIODevice::ReadOnly))
        return lexicon;
    const QJsonArray entries = QJsonDocument::fromJson(in.readAll()).object().value("entries").toArray();
    for (const auto &value : entries) {
        const QJsonObject entry = value.toObject();
        lexicon.set(entry.value("written").toString(), entry.value("spoken").toString());
    }
    return lexicon;
}

bool Lexicon::save(const QString &file) const {
    const QString path = file.isEmpty() ? defaultFile() : file;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonArray entries;
    for (const auto &entry : m_entries)
        entries.append(QJsonObject{{"written", entry.written}, {"spoken", entry.spoken}});
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(QJsonDocument(QJsonObject{{"entries", entries}}).toJson(QJsonDocument::Indented));
    return out.commit();
}

int Lexicon::indexOf(const QString &written) const {
    const QString k = key(written);
    for (int i = 0; i < m_entries.size(); ++i)
        if (key(m_entries.at(i).written) == k)
            return i;
    return -1;
}

bool Lexicon::set(const QString &written, const QString &spoken) {
    if (!valid(written, spoken))
        return false;
    const Respelling entry{written.simplified(), spoken.simplified()};
    const int at = indexOf(entry.written);
    if (at >= 0)
        m_entries[at] = entry;
    else
        m_entries.append(entry);
    m_pattern.reset();
    return true;
}

bool Lexicon::remove(const QString &written) {
    const int at = indexOf(written);
    if (at < 0)
        return false;
    m_entries.removeAt(at);
    m_pattern.reset();
    return true;
}

void Lexicon::compile() const {
    m_groupEntry.clear();
    if (m_entries.isEmpty()) {
        m_pattern.reset();
        return;
    }
    // Longest first, since at any one place the first alternative that
    // matches is taken: "New York City" before "New York".
    QList<int> order;
    for (int i = 0; i < m_entries.size(); ++i)
        order.append(i);
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
        return m_entries.at(a).written.size() > m_entries.at(b).written.size();
    });
    QStringList alternatives;
    for (const int i : std::as_const(order)) {
        const QString &written = m_entries.at(i).written;
        const QString body = patternFor(written);
        alternatives.append(capitals(written) ? "((?-i:" + body + "))" : "(" + body + ")");
        m_groupEntry.append(i);
    }
    const QString word = QStringLiteral("[\\p{L}\\p{N}\\p{M}]");
    m_pattern = std::make_shared<const QRegularExpression>(
        "(?<!" + word + ")(?:" + alternatives.join('|') + ")(?!" + word + ")",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
}

QList<Lexicon::Match> Lexicon::find(const QString &text) const {
    QList<Match> found;
    if (m_entries.isEmpty())
        return found;
    if (!m_pattern)
        compile();
    if (!m_pattern || !m_pattern->isValid())
        return found;
    auto it = m_pattern->globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        if (match.capturedLength() == 0)
            continue;
        for (int group = 1; group <= m_groupEntry.size(); ++group) {
            if (match.capturedStart(group) < 0)
                continue;
            found.append({int(match.capturedStart()), int(match.capturedEnd()),
                          m_entries.at(m_groupEntry.at(group - 1)).spoken});
            break;
        }
    }
    return found;
}

namespace Pronounce {

Utterance utterance(const Book &book, int index, const Lexicon &lexicon) {
    Utterance u;
    if (index < 0 || index >= book.sentenceCount())
        return u;
    const Sentence &s = book.sentences.at(index);
    const QStringList shown = book.sentenceWords(index);
    auto asWritten = [&] {
        u.text = book.sentenceText(index);
        u.words = shown;
        for (int i = 0; i < shown.size(); ++i)
            u.heard.append({i, i});
        return u;
    };
    if (lexicon.isEmpty() || shown.isEmpty())
        return asWritten();

    const QString slice = book.chapters.at(s.chapter).paragraphs.at(s.paragraph).text.mid(s.start, s.end - s.start);
    QList<QPair<int, int>> skipped;
    for (const auto &gap : s.skipped)
        skipped.append({gap.first - s.start, gap.second - s.start});
    // Matched with the clutter blanked out, as the words were found, and
    // never across it, so a respelling never swallows a skipped range.
    QString blanked = slice;
    for (const auto &gap : std::as_const(skipped))
        blanked.replace(gap.first, gap.second - gap.first, QString(gap.second - gap.first, ' '));
    QList<Lexicon::Match> found;
    for (const auto &match : lexicon.find(blanked)) {
        const bool overlaps = std::any_of(skipped.begin(), skipped.end(), [&](const QPair<int, int> &gap) {
            return gap.first < match.end && gap.second > match.start;
        });
        if (!overlaps)
            found.append(match);
    }
    if (found.isEmpty())
        return asWritten();

    // The text: each match respelled, from the last back so the earlier
    // offsets hold, and the skipped ranges after it moved along with it.
    QString text = slice;
    for (int m = int(found.size()) - 1; m >= 0; --m) {
        const auto &match = found.at(m);
        text.replace(match.start, match.end - match.start, match.spoken);
        const int delta = int(match.spoken.size()) - (match.end - match.start);
        for (auto &gap : skipped)
            if (gap.first >= match.end) {
                gap.first += delta;
                gap.second += delta;
            }
    }
    text = Text::withoutRanges(text, skipped);
    text.replace('\n', ' ');
    u.text = text.simplified();

    // The words: page words a match touches, together with every other
    // page word and match those touch in turn, are heard as the words of
    // that stretch respelled ("Hermione's" as "Her-my-oh-nee's").
    const QList<WordSpan> &spans = s.words;
    auto wordStart = [&](int i) { return spans.at(i).start - s.start; };
    auto wordEnd = [&](int i) { return spans.at(i).start - s.start + spans.at(i).length; };
    int m = 0;
    for (int i = 0; i < spans.size();) {
        while (m < found.size() && found.at(m).end <= wordStart(i))
            ++m;
        if (m >= found.size() || found.at(m).start >= wordEnd(i)) {
            u.heard.append({int(u.words.size()), int(u.words.size())});
            u.words.append(shown.at(i));
            ++i;
            continue;
        }
        const int from = qMin(wordStart(i), found.at(m).start);
        int to = wordEnd(i);
        int j = i + 1, n = m;
        for (bool grew = true; grew;) {
            grew = false;
            for (; n < found.size() && found.at(n).start < to; ++n, grew = true)
                to = qMax(to, found.at(n).end);
            for (; j < spans.size() && wordStart(j) < to; ++j, grew = true)
                to = qMax(to, wordEnd(j));
        }
        QString piece;
        int at = from;
        for (int k = m; k < n; ++k) {
            piece += slice.mid(at, found.at(k).start - at) + found.at(k).spoken;
            at = found.at(k).end;
        }
        piece += slice.mid(at, to - at);
        QStringList said;
        for (const auto &w : Text::words(piece, 0, int(piece.size())))
            said.append(piece.mid(w.start, w.length));
        if (said.isEmpty())
            said = shown.mid(i, j - i);
        const int first = int(u.words.size());
        u.words += said;
        for (int k = i; k < j; ++k)
            u.heard.append({first, int(u.words.size()) - 1});
        i = j;
        m = n;
    }
    return u;
}

QList<WordTiming> timings(const Utterance &utterance, const QStringList &pageWords, const QList<SpokenWord> &spoken,
                          double duration) {
    const int count = int(utterance.heard.size());
    if (count == 0 || pageWords.size() != count)
        return Speech::align(pageWords, spoken, duration);
    const QList<WordTiming> said = Speech::align(utterance.words, spoken, duration);
    QList<WordTiming> out;
    out.reserve(count);
    for (int i = 0; i < count;) {
        const auto range = utterance.heard.at(i);
        int j = i + 1;
        while (j < count && utterance.heard.at(j) == range)
            ++j;
        const double from = said.value(range.first).start;
        const double to = qMax(from, said.value(range.second).end);
        double weight = 0;
        for (int k = i; k < j; ++k)
            weight += pageWords.at(k).size() + 1;
        double cursor = from;
        for (int k = i; k < j; ++k) {
            const double length = weight > 0 ? (to - from) * (pageWords.at(k).size() + 1) / weight : 0;
            out.append({cursor, cursor + length});
            cursor += length;
        }
        i = j;
    }
    return out;
}

}

Pronunciations::Pronunciations(const QString &file, QObject *parent)
    : QObject(parent), m_file(file.isEmpty() ? Lexicon::defaultFile() : file), m_lexicon(Lexicon::load(m_file)) {}

QVariantList Pronunciations::entries() const {
    QList<Respelling> sorted = m_lexicon.entries();
    std::stable_sort(sorted.begin(), sorted.end(), [](const Respelling &a, const Respelling &b) {
        return QString::localeAwareCompare(a.written, b.written) < 0;
    });
    QVariantList list;
    for (const auto &entry : std::as_const(sorted))
        list.append(QVariantMap{{"written", entry.written}, {"spoken", entry.spoken}});
    return list;
}

QString Pronunciations::spokenFor(const QString &written) const {
    const int at = m_lexicon.indexOf(written);
    return at >= 0 ? m_lexicon.entries().at(at).spoken : QString();
}

bool Pronunciations::set(const QString &written, const QString &spoken, const QString &original) {
    if (!Lexicon::valid(written, spoken))
        return false;
    Lexicon lexicon = Lexicon::load(m_file);
    if (!original.isEmpty() && Lexicon::key(original) != Lexicon::key(written))
        lexicon.remove(original);
    lexicon.set(written, spoken);
    if (!lexicon.save(m_file))
        return false;
    m_lexicon = lexicon;
    emit changed();
    return true;
}

bool Pronunciations::remove(const QString &written) {
    Lexicon lexicon = Lexicon::load(m_file);
    const bool removed = lexicon.remove(written);
    if (removed && !lexicon.save(m_file))
        return false;
    m_lexicon = lexicon;
    emit changed();
    return removed;
}

void Pronunciations::reload() {
    m_lexicon = Lexicon::load(m_file);
    emit changed();
}

// Compiled as part of this file, not on its own; see omareader.pro.
#include "moc_pronunciation.cpp"
