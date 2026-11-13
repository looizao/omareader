#pragma once

#include <QColor>
#include <QFont>
#include <QObject>
#include <QPointer>
#include <QQuickTextDocument>
#include <QVariantList>

class QTextDocument;

// Colours the sentence being read and the word being spoken inside a
// paragraph's TextEdit, over the paragraph's own inline styles (bold, italic,
// code), and fades the text around them when focus mode asks for it. The
// styles are laid on once with the text; the highlight changes only colours,
// never fonts, so the layout stays put while it moves.
class ReadingHighlight : public QObject {
    Q_OBJECT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY documentChanged)
    // The paragraph's styles, as Reader lists them: {start, length, flags}
    // with TextStyle's flags.
    Q_PROPERTY(QVariantList styles READ styles WRITE setStyles NOTIFY stylesChanged)
    Q_PROPERTY(int sentenceStart MEMBER m_sentenceStart WRITE setSentenceStart NOTIFY rangeChanged)
    Q_PROPERTY(int sentenceEnd MEMBER m_sentenceEnd WRITE setSentenceEnd NOTIFY rangeChanged)
    Q_PROPERTY(int wordStart MEMBER m_wordStart WRITE setWordStart NOTIFY rangeChanged)
    Q_PROPERTY(int wordEnd MEMBER m_wordEnd WRITE setWordEnd NOTIFY rangeChanged)
    Q_PROPERTY(QColor sentenceColor MEMBER m_sentenceColor WRITE setSentenceColor NOTIFY rangeChanged)
    Q_PROPERTY(QColor wordColor MEMBER m_wordColor WRITE setWordColor NOTIFY rangeChanged)
    Q_PROPERTY(QColor wordTextColor MEMBER m_wordTextColor WRITE setWordTextColor NOTIFY rangeChanged)
    // The text before readEnd has been read and takes readColor; with a
    // quietColor, the text outside the sentence takes that. Transparent
    // colours leave the text as it is.
    Q_PROPERTY(int readEnd MEMBER m_readEnd WRITE setReadEnd NOTIFY rangeChanged)
    Q_PROPERTY(QColor readColor MEMBER m_readColor WRITE setReadColor NOTIFY rangeChanged)
    Q_PROPERTY(QColor quietColor MEMBER m_quietColor WRITE setQuietColor NOTIFY rangeChanged)
    // TextEdit has no lineHeight of its own; this sets it on the document,
    // as a multiple of the font's line spacing. Every line gets that same
    // height, so a symbol drawn from a taller fallback font, or a colour
    // laid over part of the text, can't push the lines apart.
    Q_PROPERTY(double lineHeight MEMBER m_lineHeight WRITE setLineHeight NOTIFY rangeChanged)
    // The text's font, which the line height is measured from.
    Q_PROPERTY(QFont font MEMBER m_font WRITE setFont NOTIFY rangeChanged)

public:
    explicit ReadingHighlight(QObject *parent = nullptr);

    QQuickTextDocument *document() const { return m_document; }
    void setDocument(QQuickTextDocument *document);
    QVariantList styles() const { return m_styles; }
    void setStyles(const QVariantList &styles);
    void setSentenceStart(int v) { set(m_sentenceStart, v); }
    void setSentenceEnd(int v) { set(m_sentenceEnd, v); }
    void setWordStart(int v) { set(m_wordStart, v); }
    void setWordEnd(int v) { set(m_wordEnd, v); }
    void setSentenceColor(const QColor &c) { set(m_sentenceColor, c); }
    void setWordColor(const QColor &c) { set(m_wordColor, c); }
    void setWordTextColor(const QColor &c) { set(m_wordTextColor, c); }
    void setReadEnd(int v) { set(m_readEnd, v); }
    void setReadColor(const QColor &c) { set(m_readColor, c); }
    void setQuietColor(const QColor &c) { set(m_quietColor, c); }
    void setLineHeight(double v) {
        if (qFuzzyCompare(v, m_lineHeight))
            return;
        m_lineHeight = v;
        emit rangeChanged();
        refresh();
    }

    void setFont(const QFont &font) {
        if (font == m_font)
            return;
        m_font = font;
        emit rangeChanged();
        refresh();
    }

    Q_INVOKABLE void refresh();
    // Each line of the text as it is laid out: {y, baseline, start}, its
    // top, its baseline and the position of its first character, so the
    // reading glass can sit on the letters. The text item's own cursor
    // rectangles don't say where a line's letters are drawn when the line
    // height is stretched.
    Q_INVOKABLE QVariantList lines() const;

signals:
    void documentChanged();
    void stylesChanged();
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
    // Sets the paragraph's own formatting back on the characters from start
    // to end, clearing whatever was there.
    void restyle(int start, int end);
    QTextDocument *textDocument() const;
    QFont textFont() const;

    QPointer<QQuickTextDocument> m_document;
    QVariantList m_styles;
    int m_sentenceStart = -1, m_sentenceEnd = -1, m_wordStart = -1, m_wordEnd = -1;
    QColor m_sentenceColor, m_wordColor, m_wordTextColor;
    int m_readEnd = -1;
    QColor m_readColor, m_quietColor;
    double m_lineHeight = 1.0;
    QFont m_font;
    // Where the characters the font lacks are, as [start, end) ranges.
    QList<std::pair<int, int>> m_fallback;
    int m_appliedStart = -1, m_appliedEnd = -1;
    bool m_applying = false;
    QMetaObject::Connection m_contents;
};
