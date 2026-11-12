#pragma once

#include "book.h"

#include <QFutureWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

class Exporter;
class FilePicker;
class Library;
class Narrator;

// The open book and what the window shows of it: the current chapter's
// paragraphs, the reading position, and the reading appearance. Exposed to
// QML as `reader`.
class Reader : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasBook READ hasBook NOTIFY bookChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString path READ path NOTIFY bookChanged)
    Q_PROPERTY(QString title READ title NOTIFY bookChanged)
    Q_PROPERTY(QString author READ author NOTIFY bookChanged)
    Q_PROPERTY(QStringList chapterTitles READ chapterTitles NOTIFY bookChanged)
    Q_PROPERTY(int chapterCount READ chapterCount NOTIFY bookChanged)
    Q_PROPERTY(int chapter READ chapter NOTIFY chapterChanged)
    Q_PROPERTY(QString chapterTitle READ chapterTitle NOTIFY chapterChanged)
    Q_PROPERTY(QVariantList paragraphs READ paragraphs NOTIFY chapterChanged)
    Q_PROPERTY(int anchorParagraph READ anchorParagraph NOTIFY chapterChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(int minutesLeftInChapter READ minutesLeftInChapter NOTIFY progressChanged)
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY appearanceChanged)
    Q_PROPERTY(double lineHeight READ lineHeight WRITE setLineHeight NOTIFY appearanceChanged)
    Q_PROPERTY(int contentWidth READ contentWidth WRITE setContentWidth NOTIFY appearanceChanged)
    Q_PROPERTY(bool serif READ serif WRITE setSerif NOTIFY appearanceChanged)
    // Aids for keeping the eyes on the words being read. Focus mode turns
    // the chosen ones on together; the highlight settings apply always.
    Q_PROPERTY(bool focusMode READ focusMode WRITE setFocusMode NOTIFY focusChanged)
    // "off", "paragraph" or "sentence": what stays lit while the rest dims.
    Q_PROPERTY(QString spotlight READ spotlight WRITE setSpotlight NOTIFY focusChanged)
    // Fade the text already read, so the eye is not pulled back to it.
    Q_PROPERTY(bool fadeRead READ fadeRead WRITE setFadeRead NOTIFY focusChanged)
    // Keep the spoken line at one height and scroll the text under it.
    Q_PROPERTY(bool steadyLine READ steadyLine WRITE setSteadyLine NOTIFY focusChanged)
    // Hide the bars while the voice plays, until the mouse or a key wakes them.
    Q_PROPERTY(bool hideControls READ hideControls WRITE setHideControls NOTIFY focusChanged)
    // "both", "word" or "sentence": what the highlight marks.
    Q_PROPERTY(QString highlight READ highlight WRITE setHighlight NOTIFY focusChanged)
    // "subtle", "normal" or "strong".
    Q_PROPERTY(QString highlightStrength READ highlightStrength WRITE setHighlightStrength NOTIFY focusChanged)

public:
    Reader(Library *library, Narrator *narrator, FilePicker *picker, QObject *parent = nullptr);
    ~Reader() override;

    bool hasBook() const { return m_book != nullptr; }
    bool loading() const { return m_loading; }
    QString error() const { return m_error; }
    QString path() const { return m_book ? m_book->path : QString(); }
    QString title() const { return m_book ? m_book->title : QString(); }
    QString author() const { return m_book ? m_book->author : QString(); }
    QStringList chapterTitles() const;
    int chapterCount() const { return m_book ? int(m_book->chapters.size()) : 0; }
    int chapter() const { return m_chapter; }
    QString chapterTitle() const;
    QVariantList paragraphs() const { return m_paragraphs; }
    int anchorParagraph() const { return m_anchorParagraph; }
    double progress() const;
    int minutesLeftInChapter() const;
    BookPtr book() const { return m_book; }
    int sentence() const { return m_sentence; }

    int fontSize() const { return m_fontSize; }
    double lineHeight() const { return m_lineHeight; }
    int contentWidth() const { return m_contentWidth; }
    bool serif() const { return m_serif; }
    void setFontSize(int size);
    void setLineHeight(double height);
    void setContentWidth(int width);
    void setSerif(bool serif);

    bool focusMode() const { return m_focusMode; }
    QString spotlight() const { return m_spotlight; }
    bool fadeRead() const { return m_fadeRead; }
    bool steadyLine() const { return m_steadyLine; }
    bool hideControls() const { return m_hideControls; }
    QString highlight() const { return m_highlight; }
    QString highlightStrength() const { return m_highlightStrength; }
    void setFocusMode(bool on);
    void setSpotlight(const QString &spotlight);
    void setFadeRead(bool on);
    void setSteadyLine(bool on);
    void setHideControls(bool on);
    void setHighlight(const QString &highlight);
    void setHighlightStrength(const QString &strength);

    Q_INVOKABLE void open(const QString &pathOrUrl);
    Q_INVOKABLE void openDialog();
    // Saves pasted text to the library and opens it; false when there is
    // nothing to save. A web page's text keeps its address and byline.
    Q_INVOKABLE bool addText(const QString &title, const QString &text, const QString &source = {},
                             const QString &author = {});
    Q_INVOKABLE void closeBook();
    Q_INVOKABLE void goToChapter(int chapter);
    Q_INVOKABLE void nextChapter();
    Q_INVOKABLE void previousChapter();
    // The paragraph at the top of the view, as the user scrolls.
    Q_INVOKABLE void setVisibleParagraph(int paragraph);
    // Starts reading aloud at a clicked character.
    Q_INVOKABLE void readFrom(int paragraph, int offset);
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void clearError();
    // Exports the open book as audio with the narrator's voice and speed and
    // the exporter's remembered layout, format and folder.
    void setExporter(Exporter *exporter) { m_exporter = exporter; }
    Q_INVOKABLE void exportAudio();
    // Roughly how long the whole book takes to hear at this speed.
    Q_INVOKABLE int audioMinutes(double speed) const;
    Q_INVOKABLE static bool isUrlOrPath(const QString &text);

    void openBook(BookPtr book); // for tests and the loader
    void saveNow();

signals:
    void bookChanged();
    void loadingChanged();
    void errorChanged();
    void chapterChanged();
    void progressChanged();
    void appearanceChanged();
    void focusChanged();

private:
    static QStringList spotlights() { return {"off", "paragraph", "sentence"}; }
    static QStringList highlights() { return {"both", "word", "sentence"}; }
    static QStringList strengths() { return {"subtle", "normal", "strong"}; }
    // Stores a focus setting and announces it, when it changed.
    template <typename T> void setFocusSetting(T &member, const T &value, const char *key);
    void setChapter(int chapter, int anchorParagraph);
    void setError(const QString &error);
    void followNarrator();

    Library *m_library;
    Narrator *m_narrator;
    FilePicker *m_picker;
    Exporter *m_exporter = nullptr;
    BookPtr m_book;
    QFutureWatcher<std::pair<std::shared_ptr<Book>, QString>> m_loader;
    QString m_loadingPath;
    bool m_loading = false;
    QString m_error;
    int m_chapter = 0;
    int m_anchorParagraph = 0;
    int m_sentence = 0;
    QVariantList m_paragraphs;
    QTimer m_saveTimer;

    int m_fontSize = 20;
    double m_lineHeight = 1.5;
    int m_contentWidth = 720;
    bool m_serif = true;

    bool m_focusMode = false;
    QString m_spotlight = QStringLiteral("paragraph");
    bool m_fadeRead = true;
    bool m_steadyLine = true;
    bool m_hideControls = true;
    QString m_highlight = QStringLiteral("both");
    QString m_highlightStrength = QStringLiteral("normal");
};
