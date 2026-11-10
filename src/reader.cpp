#include "reader.h"

#include "bookloader.h"
#include "exporter.h"
#include "filepicker.h"
#include "library.h"
#include "narrator.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QUrl>
#include <QtConcurrent>
#include <cmath>

Reader::Reader(Library *library, Narrator *narrator, FilePicker *picker, QObject *parent)
    : QObject(parent), m_library(library), m_narrator(narrator), m_picker(picker) {
    QSettings settings;
    m_fontSize = qBound(12, settings.value("reader/fontSize", 20).toInt(), 48);
    m_lineHeight = qBound(1.0, settings.value("reader/lineHeight", 1.5).toDouble(), 2.4);
    m_contentWidth = qBound(420, settings.value("reader/contentWidth", 720).toInt(), 1600);
    m_serif = settings.value("reader/serif", true).toBool();

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(800);
    connect(&m_saveTimer, &QTimer::timeout, this, &Reader::saveNow);

    connect(&m_loader, &QFutureWatcher<std::pair<std::shared_ptr<Book>, QString>>::finished, this, [this] {
        const auto [book, error] = m_loader.result();
        m_loading = false;
        emit loadingChanged();
        if (!book) {
            setError(error);
            return;
        }
        openBook(book);
    });

    if (m_picker) {
        connect(m_picker, &FilePicker::openSelected, this, [this](const QUrl &url) { open(url.toString()); });
        connect(m_picker, &FilePicker::failed, this, &Reader::setError);
    }
    if (m_narrator)
        connect(m_narrator, &Narrator::positionChanged, this, &Reader::followNarrator);
    if (m_library)
        connect(m_library, &Library::positionSynced, this, &Reader::followSync);
}

void Reader::followSync(const QString &path) {
    // While the voice reads, this device's place is the newer one anyway, and
    // pulling the view away would lose the listener.
    if (!m_book || m_book->path != path || (m_narrator && m_narrator->active()))
        return;
    m_saveTimer.stop();
    const LibraryEntry entry = m_library->entry(path);
    if (entry.chapter < 0 || entry.chapter >= m_book->chapters.size())
        return;
    const int sentence = m_book->sentenceAt(entry.chapter, entry.paragraph, qMax(0, entry.offset));
    if (sentence < 0)
        return;
    m_sentence = sentence;
    setChapter(entry.chapter, m_book->sentences.at(sentence).paragraph);
    emit progressChanged();
    emit movedBySync();
}

Reader::~Reader() {
    if (m_loader.isRunning())
        m_loader.waitForFinished();
    saveNow();
}

QStringList Reader::chapterTitles() const {
    QStringList titles;
    if (m_book)
        for (const auto &c : m_book->chapters)
            titles.append(c.title);
    return titles;
}

QString Reader::chapterTitle() const {
    return m_book && m_chapter < m_book->chapters.size() ? m_book->chapters.at(m_chapter).title : QString();
}

double Reader::progress() const {
    if (!m_book || m_book->sentenceCount() < 2)
        return 0;
    return qBound(0.0, double(m_sentence) / (m_book->sentenceCount() - 1), 1.0);
}

int Reader::audioMinutes(double speed) const {
    if (!m_book)
        return 0;
    // Both voices speak about 160 words a minute at normal speed.
    return int(std::ceil(m_book->wordCount() / (160.0 * qBound(0.5, speed, 2.0))));
}

void Reader::exportAudio() {
    if (!m_book || !m_exporter || m_exporter->running())
        return;
    Exporter::Options options;
    // The export keeps a copy of the voice as it is now; changing the voice
    // while it runs only affects listening.
    if (m_narrator) {
        m_narrator->refreshSetup();
        options.synthesis = m_narrator->synthesisOptions();
    } else {
        options.synthesis = Synthesis::optionsFor(VoiceSettings::loadSelected());
    }
    options.layout = Exporter::Layout(m_exporter->layout());
    options.format = Exporter::Format(m_exporter->format());
    options.folder = m_exporter->folder();
    m_exporter->start(m_book, options);
}

int Reader::minutesLeftInChapter() const {
    if (!m_book || m_chapter >= m_book->chapterFirstSentence.size())
        return 0;
    const int end = m_chapter + 1 < m_book->chapterFirstSentence.size() ? m_book->chapterFirstSentence.at(m_chapter + 1)
                                                                         : m_book->sentenceCount();
    int words = 0;
    for (int i = qMax(m_sentence, m_book->chapterFirstSentence.at(m_chapter)); i < end; ++i)
        words += int(m_book->sentences.at(i).words.size());
    // Both voices speak about 160 words a minute at normal speed.
    const double speed = m_narrator ? m_narrator->speed() : 1.0;
    return int(std::ceil(words / (160.0 * speed)));
}

bool Reader::isUrlOrPath(const QString &text) {
    return text.startsWith("file:") || text.startsWith('/') || text.startsWith('~');
}

void Reader::open(const QString &pathOrUrl) {
    QString path = pathOrUrl.trimmed();
    if (path.startsWith("file:"))
        path = QUrl(path).toLocalFile();
    else if (path.startsWith("~/"))
        path = QDir::homePath() + path.mid(1);
    if (path.isEmpty())
        return;
    path = QFileInfo(path).absoluteFilePath();
    if (m_loading)
        return;
    if (m_book && m_book->path == path)
        return;
    setError({});
    m_loadingPath = path;
    m_loading = true;
    emit loadingChanged();
    m_loader.setFuture(QtConcurrent::run([path] {
        QString error;
        auto book = BookLoader::load(path, &error);
        return std::make_pair(book, error);
    }));
}

void Reader::openDialog() {
    if (m_picker)
        m_picker->openBook();
}

bool Reader::addText(const QString &title, const QString &text) {
    if (!m_library || m_loading)
        return false;
    const QString path = m_library->addText(title, text);
    if (path.isEmpty()) {
        if (!text.trimmed().isEmpty())
            setError(tr("The text couldn't be saved in %1.").arg(m_library->textDirectory()));
        return false;
    }
    open(path);
    return true;
}

void Reader::openBook(BookPtr book) {
    saveNow();
    if (m_narrator)
        m_narrator->setBook(book);
    m_book = std::move(book);
    int chapter = 0, paragraph = 0;
    m_sentence = 0;
    if (m_library && m_book) {
        const bool known = m_library->contains(m_book->path);
        const LibraryEntry entry = m_library->entry(m_book->path);
        m_library->opened(*m_book);
        // Resume where reading stopped, if the book still has that place.
        if (known && entry.chapter < m_book->chapters.size()) {
            chapter = entry.chapter;
            paragraph = qBound(0, entry.paragraph, int(m_book->chapters.at(chapter).paragraphs.size()) - 1);
            // The offset finds the exact sentence, including a place synced
            // from another device; older entries only have the sentence.
            if (entry.offset >= 0)
                m_sentence = qMax(0, m_book->sentenceAt(chapter, paragraph, entry.offset));
            else
                m_sentence = entry.sentenceCount == m_book->sentenceCount()
                                 ? qBound(0, entry.sentence, m_book->sentenceCount() - 1)
                                 : qMax(0, m_book->sentenceAt(chapter, paragraph));
        }
    }
    emit bookChanged();
    setChapter(chapter, paragraph);
    emit progressChanged();
}

void Reader::closeBook() {
    saveNow();
    if (m_narrator) {
        m_narrator->stop();
        m_narrator->setBook(nullptr);
    }
    m_book.reset();
    m_paragraphs.clear();
    m_chapter = 0;
    m_anchorParagraph = 0;
    emit bookChanged();
    emit chapterChanged();
}

void Reader::setChapter(int chapter, int anchorParagraph) {
    if (!m_book)
        return;
    chapter = qBound(0, chapter, int(m_book->chapters.size()) - 1);
    m_chapter = chapter;
    m_anchorParagraph = anchorParagraph;
    m_paragraphs.clear();
    for (const auto &p : m_book->chapters.at(chapter).paragraphs) {
        QVariantList styles;
        for (const TextStyle &style : p.styles)
            styles.append(QVariantMap{{"start", style.start}, {"length", style.length}, {"flags", style.flags}});
        m_paragraphs.append(QVariantMap{{"text", p.text}, {"level", p.level}, {"styles", styles}});
    }
    emit chapterChanged();
}

void Reader::goToChapter(int chapter) {
    if (!m_book || chapter < 0 || chapter >= m_book->chapters.size())
        return;
    // Jumping elsewhere ends the narration rather than pulling the view back.
    if (m_narrator && m_narrator->active() && m_narrator->chapter() != chapter)
        m_narrator->stop();
    m_sentence = qMax(0, m_book->chapterFirstSentence.value(chapter));
    setChapter(chapter, 0);
    emit progressChanged();
    m_saveTimer.start();
}

void Reader::nextChapter() { goToChapter(m_chapter + 1); }
void Reader::previousChapter() { goToChapter(m_chapter - 1); }

void Reader::setVisibleParagraph(int paragraph) {
    if (!m_book || (m_narrator && m_narrator->active()))
        return;
    const int current = m_sentence < m_book->sentenceCount() ? m_book->sentences.at(m_sentence).paragraph : -1;
    const bool sameChapter = m_sentence < m_book->sentenceCount() && m_book->sentences.at(m_sentence).chapter == m_chapter;
    if (sameChapter && current == paragraph)
        return;
    const int sentence = m_book->sentenceAt(m_chapter, paragraph);
    if (sentence < 0)
        return;
    m_sentence = sentence;
    m_anchorParagraph = paragraph;
    emit progressChanged();
    m_saveTimer.start();
}

void Reader::readFrom(int paragraph, int offset) {
    if (!m_book || !m_narrator)
        return;
    const int sentence = m_book->sentenceAt(m_chapter, paragraph, offset);
    if (sentence < 0)
        return;
    m_sentence = sentence;
    m_narrator->play(sentence);
}

void Reader::togglePlay() {
    if (!m_book || !m_narrator)
        return;
    // Starting fresh reads from the saved place when it's on this chapter,
    // otherwise from the top of the chapter in view.
    int from = m_sentence;
    if (from >= m_book->sentenceCount() || m_book->sentences.at(from).chapter != m_chapter)
        from = qMax(0, m_book->sentenceAt(m_chapter, m_anchorParagraph));
    m_narrator->toggle(from);
}

void Reader::followNarrator() {
    if (!m_book || !m_narrator || !m_narrator->active())
        return;
    const int sentence = m_narrator->sentence();
    if (sentence < 0 || sentence >= m_book->sentenceCount())
        return;
    m_sentence = sentence;
    const Sentence &s = m_book->sentences.at(sentence);
    if (s.chapter != m_chapter)
        setChapter(s.chapter, s.paragraph);
    m_anchorParagraph = s.paragraph;
    emit progressChanged();
    m_saveTimer.start();
}

void Reader::saveNow() {
    m_saveTimer.stop();
    if (!m_book || !m_library || m_sentence >= m_book->sentenceCount())
        return;
    const Sentence &s = m_book->sentences.at(m_sentence);
    // A place inside this sentence, such as the word a phone stopped at, is
    // kept as it is, so the same place never syncs back as a new one.
    int offset = s.start;
    const LibraryEntry saved = m_library->entry(m_book->path);
    if (saved.offset >= 0 && saved.chapter == s.chapter && saved.paragraph == s.paragraph
        && m_book->sentenceAt(saved.chapter, saved.paragraph, saved.offset) == m_sentence)
        offset = saved.offset;
    m_library->savePosition(m_book->path, s.chapter, s.paragraph, m_sentence, m_book->sentenceCount(), offset);
}

void Reader::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}

void Reader::clearError() { setError({}); }

void Reader::setFontSize(int size) {
    size = qBound(12, size, 48);
    if (size == m_fontSize)
        return;
    m_fontSize = size;
    QSettings().setValue("reader/fontSize", size);
    emit appearanceChanged();
}

void Reader::setLineHeight(double height) {
    height = qBound(1.0, std::round(height * 10) / 10, 2.4);
    if (qFuzzyCompare(height, m_lineHeight))
        return;
    m_lineHeight = height;
    QSettings().setValue("reader/lineHeight", height);
    emit appearanceChanged();
}

void Reader::setContentWidth(int width) {
    width = qBound(420, width, 1600);
    if (width == m_contentWidth)
        return;
    m_contentWidth = width;
    QSettings().setValue("reader/contentWidth", width);
    emit appearanceChanged();
}

void Reader::setSerif(bool serif) {
    if (serif == m_serif)
        return;
    m_serif = serif;
    QSettings().setValue("reader/serif", serif);
    emit appearanceChanged();
}
