#include "rsvp.h"

#include "dialogue.h"
#include "narrator.h"
#include "reader.h"
#include "stats.h"

#include <QSettings>
#include <cmath>

namespace {

bool inSkipped(const Sentence &sentence, int position) {
    for (const auto &range : sentence.skipped)
        if (position >= range.first && position < range.second)
            return true;
    return false;
}

// Punctuation written against a word, which goes with it onto the screen:
// not a space, a letter or a digit, nor clutter the voice skips.
bool attached(const QString &text, const Sentence &sentence, int position) {
    const QChar c = text.at(position);
    return !c.isSpace() && !c.isLetterOrNumber() && !c.isMark() && !inSkipped(sentence, position);
}

// The [start, end) of a word with the punctuation before and after it.
// Punctuation between two words with no space ("well—known") goes with
// the first of them.
int tokenEnd(const QString &text, const Sentence &sentence, int word) {
    const WordSpan &span = sentence.words.at(word);
    const int limit = word + 1 < sentence.words.size() ? sentence.words.at(word + 1).start : sentence.end;
    int end = span.start + span.length;
    while (end < limit && attached(text, sentence, end))
        ++end;
    return end;
}

int tokenStart(const QString &text, const Sentence &sentence, int word) {
    const int limit = word > 0 ? tokenEnd(text, sentence, word - 1) : sentence.start;
    int start = sentence.words.at(word).start;
    while (start > limit && attached(text, sentence, start - 1))
        --start;
    return start;
}

// The punctuation between a word and the next one, or the end of its
// sentence, attached to it or standing on its own ("said — softly"),
// leaving out the clutter the voice skips.
QString punctuationAfter(const QString &text, const Sentence &sentence, int word) {
    const WordSpan &span = sentence.words.at(word);
    const int limit = word + 1 < sentence.words.size() ? sentence.words.at(word + 1).start : sentence.end;
    QString marks;
    for (int i = span.start + span.length; i < limit; ++i)
        if (!text.at(i).isSpace() && !inSkipped(sentence, i))
            marks.append(text.at(i));
    return marks;
}

// What a pause after a word is worth, from the punctuation that follows it.
double punctuationPause(const QString &after) {
    static const QString marks = QStringLiteral(",;:—–.!?…");
    for (const QChar c : after)
        if (marks.contains(c))
            return 1.5;
    return 1;
}

}

namespace Flashes {

constexpr double LongWord = 1.3;
constexpr int LongWordLetters = 8;
constexpr double SentencePause = 2;
constexpr double ParagraphPause = 2.5;
// A new chapter is a paragraph's pause twice over.
constexpr double ChapterPause = 2 * ParagraphPause;

QList<Flash> of(const Book &book, int size) {
    size = qBound(1, size, 3);
    QList<Flash> flashes;
    QList<QList<Dialogue::Range>> dialogue;
    int dialogueChapter = -1;
    for (int s = 0; s < book.sentenceCount(); ++s) {
        const Sentence &sentence = book.sentences.at(s);
        if (!sentence.speakable())
            continue;
        if (sentence.chapter != dialogueChapter) {
            dialogue = Dialogue::find(book.chapters.at(sentence.chapter).paragraphs);
            dialogueChapter = sentence.chapter;
        }
        const QString &text = book.chapters.at(sentence.chapter).paragraphs.at(sentence.paragraph).text;
        const QList<Dialogue::Part> parts = Dialogue::parts(sentence, dialogue.value(sentence.paragraph));
        auto partOf = [&](int word) {
            const int at = sentence.words.at(word).start;
            for (int p = 0; p < parts.size(); ++p)
                if (at >= parts.at(p).start && at < parts.at(p).end)
                    return p;
            return -1;
        };
        int word = 0;
        while (word < sentence.words.size()) {
            Flash flash;
            flash.sentence = s;
            flash.firstWord = word;
            const int part = partOf(word);
            while (word < sentence.words.size() && flash.wordCount < size && partOf(word) == part) {
                const bool punctuated = !punctuationAfter(text, sentence, word).isEmpty();
                ++flash.wordCount;
                ++word;
                if (punctuated)
                    break;
            }
            flash.start = tokenStart(text, sentence, flash.firstWord);
            flash.end = tokenEnd(text, sentence, word - 1);
            flashes.append(flash);
        }
    }

    // How long each stays up: a unit a word, more for a long one, and a
    // pause after the last one for its punctuation or the break after it.
    for (int i = 0; i < flashes.size(); ++i) {
        Flash &flash = flashes[i];
        const Sentence &sentence = book.sentences.at(flash.sentence);
        double weight = 0, last = 1;
        for (int w = flash.firstWord; w < flash.firstWord + flash.wordCount; ++w) {
            last = sentence.words.at(w).length >= LongWordLetters ? LongWord : 1;
            weight += last;
        }
        const QString &text = book.chapters.at(sentence.chapter).paragraphs.at(sentence.paragraph).text;
        double pause = punctuationPause(punctuationAfter(text, sentence, flash.firstWord + flash.wordCount - 1));
        if (i + 1 >= flashes.size()) {
            pause = ChapterPause;
        } else {
            const Sentence &next = book.sentences.at(flashes.at(i + 1).sentence);
            if (next.chapter != sentence.chapter)
                pause = ChapterPause;
            else if (next.paragraph != sentence.paragraph)
                pause = ParagraphPause;
            else if (flashes.at(i + 1).sentence != flash.sentence && sentence.ends)
                pause = SentencePause;
        }
        flash.weight = weight + last * (pause - 1);
    }
    return flashes;
}

QString text(const Book &book, const Flash &flash) {
    if (flash.sentence < 0 || flash.sentence >= book.sentenceCount())
        return {};
    const Sentence &sentence = book.sentences.at(flash.sentence);
    const QString &paragraph = book.chapters.at(sentence.chapter).paragraphs.at(sentence.paragraph).text;
    QStringList words;
    for (int w = flash.firstWord; w < flash.firstWord + flash.wordCount && w < sentence.words.size(); ++w) {
        const int start = tokenStart(paragraph, sentence, w);
        QString word = paragraph.mid(start, tokenEnd(paragraph, sentence, w) - start);
        // Invisible marks books leave in (zero-width spaces, soft hyphens)
        // would only nudge the word off its place.
        word.removeIf([](QChar c) { return c.category() == QChar::Other_Format; });
        words.append(word);
    }
    return words.join(' ');
}

int recognitionPoint(const QString &text) {
    QList<int> letters;
    for (int i = 0; i < text.size(); ++i) {
        // A letter outside the basic plane is two UTF-16 units; count its first.
        if (text.at(i).isLowSurrogate())
            continue;
        const char32_t c = text.at(i).isHighSurrogate() && i + 1 < text.size()
                               ? QChar::surrogateToUcs4(text.at(i), text.at(i + 1))
                               : text.at(i).unicode();
        if (QChar::isLetterOrNumber(c))
            letters.append(i);
    }
    if (letters.isEmpty())
        return -1;
    const int count = int(letters.size());
    const int nth = count == 1 ? 0 : count <= 5 ? 1 : count <= 9 ? 2 : count <= 13 ? 3 : 4;
    return letters.at(nth);
}

double unitMs(const QList<Flash> &flashes, int wpm) {
    double words = 0, weight = 0;
    for (const Flash &flash : flashes) {
        words += flash.wordCount;
        weight += flash.weight;
    }
    if (weight <= 0 || wpm <= 0)
        return 0;
    return 60000.0 / wpm * words / weight;
}

int durationMs(const Flash &flash, double unitMs, int sinceStart) {
    // Twice as long for the first, then a fifth less for each after it.
    const double ramp = sinceStart < RampLength ? 2.0 - double(sinceStart) / RampLength : 1.0;
    return qMax(1, int(std::lround(flash.weight * unitMs * ramp)));
}

}

Rsvp::Rsvp(Reader *reader, Narrator *narrator, ReadingStats *stats, QObject *parent)
    : QObject(parent), m_reader(reader), m_narrator(narrator), m_stats(stats) {
    QSettings settings;
    m_aloud = settings.value("rsvp/aloud", false).toBool();
    m_wpm = qBound(MinWpm, settings.value("rsvp/wpm", DefaultWpm).toInt(), MaxWpm);
    m_size = qBound(1, settings.value("rsvp/size", 1).toInt(), MaxSize);
    m_fontSize = qBound(MinFontSize, settings.value("rsvp/fontSize", 50).toInt(), MaxFontSize);

    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &Rsvp::advance);
    if (m_reader) {
        connect(m_reader, &Reader::bookChanged, this, &Rsvp::forget);
        connect(m_reader, &Reader::progressChanged, this, &Rsvp::followReader);
    }
    if (m_narrator) {
        connect(m_narrator, &Narrator::wordChanged, this, &Rsvp::followVoice);
        connect(m_narrator, &Narrator::speedChanged, this, [this] {
            if (m_aloud)
                emit flashChanged();
        });
        // The voice starting, however it was started, takes over from the
        // silent clock, and the switch says so.
        connect(m_narrator, &Narrator::stateChanged, this, [this] {
            if (!m_active || !voicePlaying())
                return;
            stopClock();
            if (!m_aloud) {
                m_aloud = true;
                QSettings().setValue("rsvp/aloud", true);
                emit settingsChanged();
                emit flashChanged();
            }
        });
    }
}

bool Rsvp::voicePlaying() const {
    return m_narrator && (m_narrator->state() == Narrator::Playing || m_narrator->state() == Narrator::Preparing);
}

void Rsvp::prepare() {
    const BookPtr book = m_reader ? m_reader->book() : nullptr;
    if (book == m_book && (!m_flashes.isEmpty() || !book))
        return;
    m_book = book;
    m_flashes = book ? Flashes::of(*book, m_size) : QList<Flash>();
    m_sentenceFirst.clear();
    m_wordsBefore.clear();
    if (book) {
        m_sentenceFirst.fill(int(m_flashes.size()), book->sentenceCount() + 1);
        for (int i = int(m_flashes.size()) - 1; i >= 0; --i)
            m_sentenceFirst[m_flashes.at(i).sentence] = i;
        // A sentence without flashes points at the next one with some.
        for (int s = book->sentenceCount() - 1; s >= 0; --s)
            if (m_sentenceFirst.at(s) >= m_flashes.size()
                || m_flashes.at(m_sentenceFirst.at(s)).sentence != s)
                m_sentenceFirst[s] = m_sentenceFirst.at(s + 1);
        m_wordsBefore.reserve(m_flashes.size() + 1);
        int words = 0;
        for (const Flash &flash : std::as_const(m_flashes)) {
            m_wordsBefore.append(words);
            words += flash.wordCount;
        }
        m_wordsBefore.append(words);
    }
    m_unitMs = Flashes::unitMs(m_flashes, m_wpm);
}

void Rsvp::forget() {
    stopClock();
    m_book.reset();
    m_flashes.clear();
    m_sentenceFirst.clear();
    m_wordsBefore.clear();
    m_index = -1;
    m_text.clear();
    m_point = -1;
    // A book always opens on the page.
    if (m_active) {
        m_active = false;
        emit activeChanged();
    }
    emit flashChanged();
    emit sentenceChanged();
}

int Rsvp::flashAt(int sentence, int word) const {
    if (m_flashes.isEmpty() || sentence < 0 || sentence >= m_sentenceFirst.size())
        return m_flashes.isEmpty() ? -1 : 0;
    int index = m_sentenceFirst.at(sentence);
    while (index + 1 < m_flashes.size() && m_flashes.at(index + 1).sentence == sentence
           && m_flashes.at(index + 1).firstWord <= word)
        ++index;
    return qMin(index, int(m_flashes.size()) - 1);
}

std::pair<int, int> Rsvp::chapterFlashes(int index) const {
    if (!m_book || index < 0 || index >= m_flashes.size())
        return {0, 0};
    const int chapter = m_book->sentences.at(m_flashes.at(index).sentence).chapter;
    const int first = m_sentenceFirst.at(m_book->chapterFirstSentence.at(chapter));
    const int end = chapter + 1 < m_book->chapterFirstSentence.size()
                        ? m_sentenceFirst.at(m_book->chapterFirstSentence.at(chapter + 1))
                        : int(m_flashes.size());
    return {first, end};
}

void Rsvp::show(int index) {
    if (m_flashes.isEmpty())
        return;
    index = qBound(0, index, int(m_flashes.size()) - 1);
    const bool sentenceMoved = m_index < 0 || m_flashes.at(m_index).sentence != m_flashes.at(index).sentence;
    const bool moved = index != m_index;
    m_index = index;
    m_text = Flashes::text(*m_book, m_flashes.at(index));
    m_point = Flashes::recognitionPoint(m_text);
    if (m_reader)
        m_reader->showSentence(m_flashes.at(index).sentence);
    if (moved)
        emit flashChanged();
    if (sentenceMoved)
        emit sentenceChanged();
}

void Rsvp::setActive(bool active) {
    if (active == m_active || (active && (!m_reader || !m_reader->book())))
        return;
    if (!active) {
        stopClock();
        m_active = false;
        if (m_reader)
            m_reader->saveNow();
        emit activeChanged();
        return;
    }
    prepare();
    if (m_flashes.isEmpty())
        return;
    m_active = true;
    // From the word being heard, or else the reading position.
    if (m_narrator && m_narrator->active() && m_narrator->book() == m_book && m_narrator->sentence() >= 0) {
        const int sentence = m_narrator->sentence();
        int word = 0;
        const auto &words = m_book->sentences.at(sentence).words;
        for (int w = 0; w < words.size(); ++w)
            if (words.at(w).start == m_narrator->wordStart())
                word = w;
        m_index = -1;
        show(flashAt(sentence, word));
        if (!m_aloud) {
            m_aloud = true;
            QSettings().setValue("rsvp/aloud", true);
            emit settingsChanged();
        }
    } else {
        m_index = -1;
        show(flashAt(m_reader->startingSentence()));
    }
    emit activeChanged();
}

void Rsvp::setAloud(bool aloud) {
    if (aloud == m_aloud)
        return;
    m_aloud = aloud;
    QSettings().setValue("rsvp/aloud", aloud);
    emit settingsChanged();
    emit flashChanged();
    if (!m_active || m_index < 0 || !m_narrator)
        return;
    if (aloud) {
        // The voice goes on from the flash's sentence, as the clock was running.
        if (running()) {
            stopClock();
            m_narrator->play(m_flashes.at(m_index).sentence);
        }
        return;
    }
    // The silent clock goes on from the word being heard.
    if (m_narrator->active()) {
        const bool wasPlaying = voicePlaying();
        followVoice();
        m_narrator->stop();
        if (wasPlaying)
            play();
    }
}

void Rsvp::setWpm(int wpm) {
    wpm = qBound(MinWpm, int(std::lround(double(wpm) / WpmStep)) * WpmStep, MaxWpm);
    if (wpm == m_wpm)
        return;
    m_wpm = wpm;
    m_unitMs = Flashes::unitMs(m_flashes, m_wpm);
    QSettings().setValue("rsvp/wpm", wpm);
    emit settingsChanged();
    emit flashChanged();
}

void Rsvp::setSize(int size) {
    size = qBound(1, size, MaxSize);
    if (size == m_size)
        return;
    m_size = size;
    QSettings().setValue("rsvp/size", size);
    if (!m_flashes.isEmpty()) {
        // The same word stays on screen, in a flash of the new size.
        const Flash at = m_flashes.value(m_index);
        m_flashes.clear();
        prepare();
        m_index = -1;
        show(flashAt(at.sentence, at.firstWord));
    }
    emit settingsChanged();
}

void Rsvp::setFontSize(int size) {
    size = qBound(MinFontSize, size, MaxFontSize);
    if (size == m_fontSize)
        return;
    m_fontSize = size;
    QSettings().setValue("rsvp/fontSize", size);
    emit settingsChanged();
}

QString Rsvp::text() const { return m_text; }
QString Rsvp::before() const { return m_point < 0 ? m_text : m_text.left(m_point); }
QString Rsvp::pivot() const {
    if (m_point < 0)
        return {};
    return m_text.mid(m_point, m_text.at(m_point).isHighSurrogate() ? 2 : 1);
}
QString Rsvp::after() const { return m_point < 0 ? QString() : m_text.mid(m_point + pivot().size()); }

int Rsvp::paragraph() const {
    return m_index >= 0 && m_book ? m_book->sentences.at(m_flashes.at(m_index).sentence).paragraph : -1;
}

int Rsvp::start() const { return m_index >= 0 ? m_flashes.at(m_index).start : -1; }

QString Rsvp::word() const {
    if (m_index < 0 || !m_book)
        return {};
    const Flash &flash = m_flashes.at(m_index);
    const Sentence &sentence = m_book->sentences.at(flash.sentence);
    const QString &text = m_book->chapters.at(sentence.chapter).paragraphs.at(sentence.paragraph).text;
    int word = flash.firstWord;
    if (m_narrator && m_narrator->active() && m_narrator->sentence() == flash.sentence)
        for (int w = flash.firstWord; w < flash.firstWord + flash.wordCount; ++w)
            if (sentence.words.at(w).start == m_narrator->wordStart())
                word = w;
    const WordSpan &span = sentence.words.at(word);
    return text.mid(span.start, span.length);
}

QVariantList Rsvp::context() const {
    QVariantList list;
    if (m_index < 0 || !m_book)
        return list;
    const int sentence = m_flashes.at(m_index).sentence;
    for (int i = m_sentenceFirst.at(sentence); i < m_flashes.size() && m_flashes.at(i).sentence == sentence; ++i)
        list.append(QVariantMap{{"index", i}, {"text", Flashes::text(*m_book, m_flashes.at(i))}});
    return list;
}

double Rsvp::chapterProgress() const {
    const auto [first, end] = chapterFlashes(m_index);
    return end - first > 1 ? double(m_index - first) / (end - first - 1) : 0;
}

int Rsvp::minutesLeftInChapter() const {
    const auto [first, end] = chapterFlashes(m_index);
    if (end <= first)
        return 0;
    const int words = m_wordsBefore.at(end) - m_wordsBefore.at(m_index);
    // Both voices speak about 160 words a minute at normal speed.
    const double rate = m_aloud ? 160.0 * (m_narrator ? m_narrator->speed() : 1.0) : double(m_wpm);
    return int(std::ceil(words / rate));
}

void Rsvp::toggle() {
    if (!m_active || m_index < 0)
        return;
    if (!m_aloud) {
        running() ? pause() : play();
        return;
    }
    if (!m_narrator)
        return;
    const int sentence = m_flashes.at(m_index).sentence;
    // Stepped away from where the voice paused: it reads from there instead.
    if (m_narrator->state() == Narrator::Paused && m_narrator->sentence() != sentence)
        m_narrator->play(sentence);
    else
        m_narrator->toggle(sentence);
}

void Rsvp::play() {
    if (!m_active || m_index < 0)
        return;
    if (m_aloud) {
        if (m_narrator && !voicePlaying())
            toggle();
        return;
    }
    startClock();
}

void Rsvp::pause() {
    if (m_aloud && m_narrator)
        m_narrator->pause();
    stopClock();
}

void Rsvp::step(int flashes) {
    if (!m_active || m_index < 0)
        return;
    stopClock();
    if (voicePlaying())
        m_narrator->pause();
    show(m_index + flashes);
}

void Rsvp::skip(int sentences) {
    if (!m_active || m_index < 0)
        return;
    if (m_narrator && m_narrator->active()) {
        m_narrator->skip(sentences);
        return;
    }
    // Like the voice: the first flash of the sentence before or after.
    int target = m_index;
    for (int moved = 0; moved < std::abs(sentences); ++moved) {
        const int first = m_sentenceFirst.at(m_flashes.at(target).sentence);
        if (sentences > 0) {
            const int next = m_sentenceFirst.at(m_flashes.at(target).sentence + 1);
            if (next >= m_flashes.size())
                return;
            target = next;
        } else {
            if (first == 0)
                return;
            target = m_sentenceFirst.at(m_flashes.at(first - 1).sentence);
        }
    }
    moveTo(target);
}

void Rsvp::moveTo(int index) {
    if (!m_active || m_flashes.isEmpty())
        return;
    show(index);
    if (running()) {
        m_timer.start(Flashes::durationMs(m_flashes.at(m_index), m_unitMs, m_sinceStart));
    }
}

void Rsvp::startClock() {
    if (m_index < 0 || m_flashes.isEmpty())
        return;
    m_sinceStart = 0;
    const bool was = running();
    m_timer.start(Flashes::durationMs(m_flashes.at(m_index), m_unitMs, m_sinceStart));
    if (m_stats)
        m_stats->noteActivity();
    if (!was)
        emit runningChanged();
}

void Rsvp::stopClock() {
    if (!running())
        return;
    m_timer.stop();
    emit runningChanged();
}

void Rsvp::advance() {
    // Reading on its own is reading time, with no key or mouse to show it.
    if (m_stats)
        m_stats->noteActivity();
    if (m_index + 1 >= m_flashes.size()) {
        // The end of the book: the last word stays, and the clock stops.
        emit runningChanged();
        return;
    }
    ++m_sinceStart;
    show(m_index + 1);
    m_timer.start(Flashes::durationMs(m_flashes.at(m_index), m_unitMs, m_sinceStart));
}

void Rsvp::followReader() {
    // The voice moves the flash word by word itself.
    if (!m_active || m_flashes.isEmpty() || !m_reader || (m_narrator && m_narrator->active()))
        return;
    const int sentence = m_reader->sentence();
    if (m_index >= 0 && m_flashes.at(m_index).sentence == sentence)
        return;
    moveTo(flashAt(sentence));
}

void Rsvp::followVoice() {
    if (!m_active || m_flashes.isEmpty() || !m_narrator || !m_narrator->active() || m_narrator->book() != m_book)
        return;
    const int sentence = m_narrator->sentence();
    const int start = m_narrator->wordStart();
    // Between words the flash stays on the last one heard.
    if (sentence < 0 || sentence >= m_book->sentenceCount() || start < 0)
        return;
    const auto &words = m_book->sentences.at(sentence).words;
    for (int w = 0; w < words.size(); ++w)
        if (words.at(w).start == start) {
            const int index = flashAt(sentence, w);
            if (index != m_index)
                show(index);
            return;
        }
}

// Compiled as part of this file, not on its own; see omareader.pro.
#include "moc_rsvp.cpp"
