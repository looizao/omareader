#pragma once

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>

class QTextDocument;

// Colours the sentence being read and the word being spoken inside a
// paragraph's TextEdit. Only colours change, never fonts, so the paragraph's
// layout stays put while the highlight moves through it.
class ReadingHighlight : public QObject {
    Q_OBJECT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(int sentenceStart MEMBER m_sentenceStart WRITE setSentenceStart NOTIFY rangeChanged)
    Q_PROPERTY(int sentenceEnd MEMBER m_sentenceEnd WRITE setSentenceEnd NOTIFY rangeChanged)
    Q_PROPERTY(int wordStart MEMBER m_wordStart WRITE setWordStart NOTIFY rangeChanged)
    Q_PROPERTY(int wordEnd MEMBER m_wordEnd WRITE setWordEnd NOTIFY rangeChanged)
    Q_PROPERTY(QColor sentenceColor MEMBER m_sentenceColor WRITE setSentenceColor NOTIFY rangeChanged)
    Q_PROPERTY(QColor wordColor MEMBER m_wordColor WRITE setWordColor NOTIFY rangeChanged)
    Q_PROPERTY(QColor wordTextColor MEMBER m_wordTextColor WRITE setWordTextColor NOTIFY rangeChanged)
    // TextEdit has no lineHeight of its own; this sets it on the document.
    Q_PROPERTY(double lineHeight MEMBER m_lineHeight WRITE setLineHeight NOTIFY rangeChanged)

public:
    explicit ReadingHighlight(QObject *parent = nullptr);

    QQuickTextDocument *document() const { return m_document; }
    void setDocument(QQuickTextDocument *document);
    void setSentenceStart(int v) { set(m_sentenceStart, v); }
    void setSentenceEnd(int v) { set(m_sentenceEnd, v); }
    void setWordStart(int v) { set(m_wordStart, v); }
    void setWordEnd(int v) { set(m_wordEnd, v); }
    void setSentenceColor(const QColor &c) { set(m_sentenceColor, c); }
    void setWordColor(const QColor &c) { set(m_wordColor, c); }
    void setWordTextColor(const QColor &c) { set(m_wordTextColor, c); }
    void setLineHeight(double v) {
        if (qFuzzyCompare(v, m_lineHeight))
            return;
        m_lineHeight = v;
        emit rangeChanged();
        refresh();
    }

    Q_INVOKABLE void refresh();

signals:
    void documentChanged();
    void rangeChanged();

private:
    template <typename T> void set(T &member, const T &value) {
        if (member == value)
            return;
        member = value;
        emit rangeChanged();
        apply();
    }
    void apply();
    QTextDocument *textDocument() const;

    QPointer<QQuickTextDocument> m_document;
    int m_sentenceStart = -1, m_sentenceEnd = -1, m_wordStart = -1, m_wordEnd = -1;
    QColor m_sentenceColor, m_wordColor, m_wordTextColor;
    double m_lineHeight = 1.0;
    int m_appliedStart = -1, m_appliedEnd = -1;
    bool m_applying = false;
    QMetaObject::Connection m_contents;
};
