#include "bookloader.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QUrl>
#include <QXmlStreamReader>
#include <private/qzipreader_p.h>

namespace {

// Named entities that turn up in real-world ebook HTML. XHTML only defines
// the five XML ones, but books are written by hand and by every tool there is.
const QHash<QString, QString> &entities() {
    static const QHash<QString, QString> table = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
        {"nbsp", QString(QChar(0x00A0))}, {"ensp", " "}, {"emsp", " "}, {"thinsp", " "},
        {"mdash", QString(QChar(0x2014))}, {"ndash", QString(QChar(0x2013))},
        {"hellip", QString(QChar(0x2026))}, {"lsquo", QString(QChar(0x2018))},
        {"rsquo", QString(QChar(0x2019))}, {"ldquo", QString(QChar(0x201C))},
        {"rdquo", QString(QChar(0x201D))}, {"sbquo", QString(QChar(0x201A))},
        {"bdquo", QString(QChar(0x201E))}, {"laquo", QString(QChar(0x00AB))},
        {"raquo", QString(QChar(0x00BB))}, {"lsaquo", QString(QChar(0x2039))},
        {"rsaquo", QString(QChar(0x203A))}, {"copy", QString(QChar(0x00A9))},
        {"reg", QString(QChar(0x00AE))}, {"trade", QString(QChar(0x2122))},
        {"deg", QString(QChar(0x00B0))}, {"middot", QString(QChar(0x00B7))},
        {"bull", QString(QChar(0x2022))}, {"sect", QString(QChar(0x00A7))},
        {"para", QString(QChar(0x00B6))}, {"dagger", QString(QChar(0x2020))},
        {"Dagger", QString(QChar(0x2021))}, {"times", QString(QChar(0x00D7))},
        {"divide", QString(QChar(0x00F7))}, {"frac12", QString(QChar(0x00BD))},
        {"frac14", QString(QChar(0x00BC))}, {"frac34", QString(QChar(0x00BE))},
        {"pound", QString(QChar(0x00A3))}, {"euro", QString(QChar(0x20AC))},
        {"cent", QString(QChar(0x00A2))}, {"yen", QString(QChar(0x00A5))},
        {"iexcl", QString(QChar(0x00A1))}, {"iquest", QString(QChar(0x00BF))},
        {"shy", QString()}, {"zwj", QString()}, {"zwnj", QString()},
        {"aacute", "á"}, {"Aacute", "Á"}, {"agrave", "à"}, {"Agrave", "À"}, {"acirc", "â"},
        {"Acirc", "Â"}, {"atilde", "ã"}, {"Atilde", "Ã"}, {"auml", "ä"}, {"Auml", "Ä"},
        {"aring", "å"}, {"Aring", "Å"}, {"aelig", "æ"}, {"AElig", "Æ"}, {"ccedil", "ç"},
        {"Ccedil", "Ç"}, {"eacute", "é"}, {"Eacute", "É"}, {"egrave", "è"}, {"Egrave", "È"},
        {"ecirc", "ê"}, {"Ecirc", "Ê"}, {"euml", "ë"}, {"Euml", "Ë"}, {"iacute", "í"},
        {"Iacute", "Í"}, {"igrave", "ì"}, {"Igrave", "Ì"}, {"icirc", "î"}, {"Icirc", "Î"},
        {"iuml", "ï"}, {"Iuml", "Ï"}, {"ntilde", "ñ"}, {"Ntilde", "Ñ"}, {"oacute", "ó"},
        {"Oacute", "Ó"}, {"ograve", "ò"}, {"Ograve", "Ò"}, {"ocirc", "ô"}, {"Ocirc", "Ô"},
        {"otilde", "õ"}, {"Otilde", "Õ"}, {"ouml", "ö"}, {"Ouml", "Ö"}, {"oslash", "ø"},
        {"Oslash", "Ø"}, {"oelig", "œ"}, {"OElig", "Œ"}, {"szlig", "ß"}, {"uacute", "ú"},
        {"Uacute", "Ú"}, {"ugrave", "ù"}, {"Ugrave", "Ù"}, {"ucirc", "û"}, {"Ucirc", "Û"},
        {"uuml", "ü"}, {"Uuml", "Ü"}, {"yacute", "ý"}, {"Yacute", "Ý"}, {"yuml", "ÿ"},
        {"thorn", "þ"}, {"THORN", "Þ"}, {"eth", "ð"}, {"ETH", "Ð"},
    };
    return table;
}

QString decodeEntity(const QString &name) {
    if (name.startsWith('#')) {
        bool ok = false;
        const uint code = name.startsWith("#x", Qt::CaseInsensitive) ? name.mid(2).toUInt(&ok, 16)
                                                                       : name.mid(1).toUInt(&ok, 10);
        if (ok && code > 0 && code <= 0x10FFFF)
            return QString::fromUcs4(reinterpret_cast<const char32_t *>(&code), 1);
        return {};
    }
    return entities().value(name, QString());
}

// Joins whitespace inside a paragraph to single spaces, keeping explicit line
// breaks (from <br>, verse lines) as newlines.
QString tidy(const QString &text) {
    QStringList lines = text.split('\n');
    for (auto &line : lines)
        line = line.simplified();
    lines.removeAll(QString());
    return lines.join('\n');
}

bool hasReadableText(const QString &text) {
    for (const QChar c : text)
        if (c.isLetterOrNumber())
            return true;
    return false;
}

QString resolvePath(const QString &base, const QString &href) {
    QString path = QUrl::fromPercentEncoding(href.section('#', 0, 0).toUtf8());
    if (path.isEmpty())
        return {};
    if (path.startsWith('/'))
        return QDir::cleanPath(path.mid(1));
    return QDir::cleanPath(base.isEmpty() ? path : base + '/' + path);
}

QString directoryOf(const QString &path) {
    const int slash = int(path.lastIndexOf('/'));
    return slash < 0 ? QString() : path.left(slash);
}

void dropEmptyChapters(QList<Chapter> &chapters) {
    chapters.removeIf([](const Chapter &c) {
        for (const auto &p : c.paragraphs)
            if (hasReadableText(p.text))
                return false;
        return true;
    });
}

// Reads the text of the element the reader is on, mapping the document
// formats' spacing elements (ODF text:s, text:tab, text:line-break; OOXML
// w:tab, w:br) to characters. Returns at the element's end tag.
QString collectRunText(QXmlStreamReader &xml) {
    QString text;
    int depth = 1;
    while (depth > 0 && !xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            ++depth;
            const auto name = xml.name();
            if (name == u"s") {
                const int count = qMax(1, xml.attributes().value("text:c").toInt());
                text += QString(count, ' ');
            } else if (name == u"tab") {
                text += ' ';
            } else if (name == u"line-break" || name == u"br" || name == u"cr") {
                text += '\n';
            } else if (name == u"note" || name == u"delText" || name == u"instrText") {
                // Footnote bodies and tracked deletions are not part of the reading text.
                xml.skipCurrentElement();
                --depth;
            }
        } else if (xml.isEndElement()) {
            --depth;
        } else if (xml.isCharacters()) {
            text += xml.text();
        }
    }
    return text;
}

}

QStringList BookLoader::nativeExtensions() {
    return {"epub", "pdf", "fb2", "docx", "odt", "txt", "text", "md", "markdown", "html", "htm", "xhtml"};
}

QStringList BookLoader::convertedExtensions() {
    return {"mobi", "azw", "azw3", "azw4", "kf8", "prc", "pdb", "lit", "lrf", "rtf", "snb", "tcr", "pml", "rb"};
}

QString BookLoader::decodeText(const QByteArray &bytes) {
    if (bytes.startsWith("\xFF\xFE") || bytes.startsWith("\xFE\xFF")) {
        QStringDecoder utf16(QStringDecoder::Utf16);
        return utf16(bytes);
    }
    // Stateless, so a broken sequence at the very end counts as an error
    // instead of waiting for bytes that will never come.
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString text = utf8(bytes);
    if (!utf8.hasError())
        return text.startsWith(QChar(0xFEFF)) ? text.mid(1) : text;
    // Old plain-text books are often Windows-1252; Latin-1 is the floor.
    QStringDecoder windows("windows-1252");
    if (windows.isValid())
        return windows(bytes);
    return QString::fromLatin1(bytes);
}

QList<Paragraph> BookLoader::htmlParagraphs(const QByteArray &html, QString *title) {
    static const QSet<QString> blocks = {
        "p", "div", "li", "ul", "ol", "dl", "dd", "dt", "blockquote", "section", "article", "header",
        "footer", "aside", "figure", "figcaption", "pre", "tr", "table", "hr", "nav", "body", "main",
        "address", "center", "caption", "tbody", "thead", "h1", "h2", "h3", "h4", "h5", "h6"};
    // Contents that are not reading text: code, styling, ruby annotations, and
    // superscript note markers that would otherwise be read out as numbers.
    static const QSet<QString> skipped = {"script", "style", "head", "svg", "math", "rt", "rp",
                                          "sup", "noscript", "template", "object"};

    const QString source = decodeText(html);
    QList<Paragraph> paragraphs;
    QString current;
    int level = 0;
    QString skipping;
    int skipDepth = 0;
    bool inTitle = false;
    QString titleText;

    auto flush = [&] {
        const QString text = tidy(current);
        if (hasReadableText(text))
            paragraphs.append({text, level});
        current.clear();
    };

    const int length = int(source.size());
    for (int i = 0; i < length; ++i) {
        const QChar c = source.at(i);
        if (c == '<') {
            if (source.mid(i, 4) == "<!--") {
                const int end = int(source.indexOf("-->", i + 4));
                i = end < 0 ? length : end + 2;
                continue;
            }
            if (source.mid(i, 9) == "<![CDATA[") {
                const int end = int(source.indexOf("]]>", i + 9));
                if (skipping.isEmpty())
                    current += source.mid(i + 9, (end < 0 ? length : end) - i - 9);
                i = end < 0 ? length : end + 2;
                continue;
            }
            const int end = int(source.indexOf('>', i + 1));
            if (end < 0)
                break;
            const QString tag = source.mid(i + 1, end - i - 1);
            i = end;
            if (tag.startsWith('!') || tag.startsWith('?'))
                continue;
            const bool closing = tag.startsWith('/');
            const bool selfClosing = tag.endsWith('/');
            static const QRegularExpression namePattern(R"(^/?\s*([A-Za-z][\w:.-]*))");
            const auto match = namePattern.match(tag);
            if (!match.hasMatch())
                continue;
            QString name = match.captured(1).toLower();
            if (name.contains(':'))
                name = name.section(':', -1);

            if (!skipping.isEmpty()) {
                if (name == skipping && !selfClosing)
                    skipDepth += closing ? -1 : 1;
                if (skipDepth == 0)
                    skipping.clear();
                if (name == "title" && title)
                    inTitle = !closing && !selfClosing;
                continue;
            }
            if (skipped.contains(name) && !closing && !selfClosing) {
                skipping = name;
                skipDepth = 1;
                continue;
            }
            if (name == "br") {
                current += '\n';
            } else if (name == "img" && !closing) {
                // An image's alt text stands in for it when it's real text
                // (drop caps are often images of a single letter).
                static const QRegularExpression alt(R"re(\balt\s*=\s*["']([^"']*)["'])re");
                const auto altMatch = alt.match(tag);
                if (altMatch.hasMatch() && altMatch.captured(1).size() == 1)
                    current += altMatch.captured(1);
            } else if (name.size() == 2 && name.at(0) == 'h' && name.at(1).isDigit() && name.at(1) != '0') {
                flush();
                level = closing ? 0 : name.at(1).digitValue();
            } else if (blocks.contains(name)) {
                flush();
            } else if (name == "td" || name == "th") {
                current += ' ';
            }
            continue;
        }

        if (inTitle) {
            titleText += c;
            continue;
        }
        if (!skipping.isEmpty())
            continue;

        if (c == '&') {
            const int semicolon = int(source.indexOf(';', i + 1));
            if (semicolon > i && semicolon - i <= 10) {
                current += decodeEntity(source.mid(i + 1, semicolon - i - 1));
                i = semicolon;
                continue;
            }
        }
        // Source line breaks are just whitespace in HTML; only <br> breaks a line.
        current += (c.isSpace() || c == QChar(0x00A0)) ? QChar(' ') : c;
    }
    flush();
    if (title)
        *title = titleText.simplified();
    return paragraphs;
}

QList<Chapter> BookLoader::textChapters(const QString &input) {
    QString text = input;
    text.replace("\r\n", "\n").replace('\r', '\n');

    // Project Gutenberg wraps books in a licence header and footer.
    static const QRegularExpression gutenbergStart(R"(\*\*\*\s*START OF (THE|THIS) PROJECT GUTENBERG[^\n]*\n)",
                                                   QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression gutenbergEnd(R"(\*\*\*\s*END OF (THE|THIS) PROJECT GUTENBERG)",
                                                 QRegularExpression::CaseInsensitiveOption);
    const auto start = gutenbergStart.match(text);
    if (start.hasMatch())
        text = text.mid(start.capturedEnd());
    const auto end = gutenbergEnd.match(text);
    if (end.hasMatch())
        text = text.left(end.capturedStart());

    static const QRegularExpression blank(R"(\n[ \t]*\n)");
    QStringList blocks = text.split(blank, Qt::SkipEmptyParts);
    // A file with no blank lines at all writes one paragraph per line.
    if (blocks.size() <= 1)
        blocks = text.split('\n', Qt::SkipEmptyParts);

    static const QRegularExpression heading(
        R"(^\s*((chapter|cap[ií]tulo|chapitre|kapitel|part|parte|book|livro|libro|prologue|pr[oó]logo|epilogue|ep[ií]logo|introduction|introdu[cç][aã]o)\b.{0,70}|[IVXLC]{1,7}\.?)\s*$)",
        QRegularExpression::CaseInsensitiveOption);

    QList<Chapter> chapters;
    chapters.append(Chapter{});
    for (const QString &block : blocks) {
        QString paragraph = block;
        paragraph.replace('\n', ' ');
        paragraph = paragraph.simplified();
        if (!hasReadableText(paragraph))
            continue;
        if (paragraph.size() <= 80 && heading.match(paragraph).hasMatch()) {
            if (!chapters.last().paragraphs.isEmpty() || !chapters.last().title.isEmpty())
                chapters.append(Chapter{});
            chapters.last().title = paragraph;
            chapters.last().paragraphs.append({paragraph, 2});
            continue;
        }
        chapters.last().paragraphs.append({paragraph, 0});
    }
    dropEmptyChapters(chapters);
    return chapters;
}

QList<Chapter> BookLoader::markdownChapters(const QString &input) {
    QString text = input;
    text.replace("\r\n", "\n");
    // YAML front matter is metadata, not reading text.
    if (text.startsWith("---\n")) {
        const int close = int(text.indexOf("\n---", 4));
        if (close > 0)
            text = text.mid(text.indexOf('\n', close + 1) + 1);
    }

    static const QRegularExpression image(R"(!\[[^\]]*\]\([^)]*\))");
    static const QRegularExpression link(R"(\[([^\]]*)\]\([^)]*\))");
    static const QRegularExpression emphasis(R"((\*{1,3}|_{1,3}|`+|~~))");
    static const QRegularExpression headingLine(R"(^(#{1,6})\s+(.*?)\s*#*\s*$)");
    static const QRegularExpression listMarker(R"(^\s*([-*+]|\d+[.)])\s+)");
    auto inline_ = [&](QString line) {
        line.replace(image, QString()).replace(link, "\\1").replace(emphasis, QString());
        return line;
    };

    QList<Chapter> chapters{Chapter{}};
    QString paragraph;
    bool fenced = false;
    auto flush = [&] {
        const QString t = paragraph.simplified();
        if (hasReadableText(t))
            chapters.last().paragraphs.append({t, 0});
        paragraph.clear();
    };
    for (QString line : text.split('\n')) {
        if (line.trimmed().startsWith("```")) {
            flush();
            fenced = !fenced;
            continue;
        }
        if (fenced)
            continue; // code blocks are skipped: they don't read aloud
        const auto heading = headingLine.match(line);
        if (heading.hasMatch()) {
            flush();
            const int level = int(heading.captured(1).size());
            const QString title = inline_(heading.captured(2)).simplified();
            if (level <= 2 && (!chapters.last().paragraphs.isEmpty() || !chapters.last().title.isEmpty()))
                chapters.append(Chapter{});
            if (level <= 2 && chapters.last().title.isEmpty())
                chapters.last().title = title;
            chapters.last().paragraphs.append({title, level});
            continue;
        }
        if (line.trimmed().isEmpty() || QRegularExpression(R"(^\s*([-*_]\s*){3,}$)").match(line).hasMatch()) {
            flush();
            continue;
        }
        if (listMarker.match(line).hasMatch()) {
            flush();
            line.remove(listMarker);
        }
        line.remove(QRegularExpression(R"(^\s*>\s?)"));
        paragraph += ' ' + inline_(line);
    }
    flush();
    dropEmptyChapters(chapters);
    return chapters;
}

QList<Chapter> BookLoader::pdfChapters(const QString &output, int pagesPerChapter) {
    QStringList pages = output.split('\f');
    while (!pages.isEmpty() && pages.last().trimmed().isEmpty())
        pages.removeLast();

    static const QRegularExpression pageNumber(R"(^\s*(\d{1,4}|[ivxlc]{1,6})\s*$)",
                                               QRegularExpression::CaseInsensitiveOption);
    auto endsSentence = [](const QString &t) {
        static const QRegularExpression end(R"([.!?…:"”’)\]]\s*$)");
        return end.match(t).hasMatch();
    };

    QList<Chapter> chapters;
    for (int pageIndex = 0; pageIndex < pages.size(); ++pageIndex) {
        if (pageIndex % pagesPerChapter == 0) {
            Chapter chapter;
            const int last = qMin(int(pages.size()), pageIndex + pagesPerChapter);
            chapter.title = last - pageIndex == 1 ? tr("Page %1").arg(pageIndex + 1)
                                                  : tr("Pages %1–%2").arg(pageIndex + 1).arg(last);
            chapters.append(chapter);
        }

        QStringList lines = pages.at(pageIndex).split('\n');
        for (auto &line : lines)
            line = line.simplified();
        // Typical line length on this page, to spot a paragraph's short last line.
        QList<int> lengths;
        for (const auto &line : lines)
            if (line.size() > 20)
                lengths.append(int(line.size()));
        std::sort(lengths.begin(), lengths.end());
        const int typical = lengths.isEmpty() ? 60 : lengths.at(lengths.size() / 2);

        QStringList paragraphs;
        QString current;
        for (int i = 0; i < lines.size(); ++i) {
            const QString &line = lines.at(i);
            if (line.isEmpty() || pageNumber.match(line).hasMatch()) {
                if (!current.isEmpty())
                    paragraphs.append(current);
                current.clear();
                continue;
            }
            if (current.endsWith('-') && !line.isEmpty() && line.at(0).isLower())
                current = current.left(current.size() - 1) + line;
            else
                current = current.isEmpty() ? line : current + ' ' + line;
            if (endsSentence(line) && line.size() < typical * 0.75) {
                paragraphs.append(current);
                current.clear();
            }
        }
        if (!current.isEmpty())
            paragraphs.append(current);

        auto &target = chapters.last().paragraphs;
        for (int i = 0; i < paragraphs.size(); ++i) {
            const QString &p = paragraphs.at(i);
            if (!hasReadableText(p))
                continue;
            // A paragraph that runs over the page break continues here.
            if (i == 0 && p.at(0).isLower()) {
                QList<Paragraph> *previous = !target.isEmpty() ? &target
                                           : chapters.size() > 1 ? &chapters[chapters.size() - 2].paragraphs
                                                                 : nullptr;
                if (previous && !previous->isEmpty() && !endsSentence(previous->last().text)) {
                    QString &tail = previous->last().text;
                    tail = tail.endsWith('-') ? tail.left(tail.size() - 1) + p : tail + ' ' + p;
                    continue;
                }
            }
            target.append({p, 0});
        }
    }
    dropEmptyChapters(chapters);
    return chapters;
}

bool BookLoader::loadEpub(Book &book, QString *error) {
    QZipReader zip(book.path);
    if (!zip.isReadable()) {
        *error = tr("This EPUB could not be opened as a zip archive.");
        return false;
    }

    QString opfPath;
    {
        QXmlStreamReader xml(zip.fileData("META-INF/container.xml"));
        while (!xml.atEnd() && opfPath.isEmpty())
            if (xml.readNextStartElement() && xml.name() == u"rootfile")
                opfPath = xml.attributes().value("full-path").toString();
    }
    if (opfPath.isEmpty()) {
        *error = tr("This EPUB has no package document (META-INF/container.xml is missing or broken).");
        return false;
    }
    const QString opfDir = directoryOf(opfPath);

    struct Item {
        QString href, type, properties;
    };
    QHash<QString, Item> manifest;
    QStringList spine;
    QString tocId, coverId;
    {
        QXmlStreamReader xml(zip.fileData(opfPath));
        while (!xml.atEnd()) {
            if (!xml.readNextStartElement())
                continue;
            const auto name = xml.name();
            const auto a = xml.attributes();
            if (name == u"title" && book.title.isEmpty()) {
                book.title = xml.readElementText().simplified();
            } else if (name == u"creator" && book.author.isEmpty()) {
                book.author = xml.readElementText().simplified();
            } else if (name == u"meta" && a.value("name") == u"cover") {
                coverId = a.value("content").toString();
            } else if (name == u"item") {
                manifest.insert(a.value("id").toString(),
                                {resolvePath(opfDir, a.value("href").toString()), a.value("media-type").toString(),
                                 a.value("properties").toString()});
            } else if (name == u"spine") {
                tocId = a.value("toc").toString();
            } else if (name == u"itemref" && a.value("linear") != u"no") {
                spine.append(a.value("idref").toString());
            }
        }
    }
    if (spine.isEmpty()) {
        *error = tr("This EPUB has no readable spine.");
        return false;
    }

    // Chapter titles come from the table of contents: the EPUB 3 nav document,
    // or the EPUB 2 NCX.
    QHash<QString, QString> tocTitles;
    for (auto it = manifest.cbegin(); it != manifest.cend(); ++it) {
        const bool nav = it->properties.split(' ').contains("nav");
        const bool ncx = it.key() == tocId || it->type == "application/x-dtbncx+xml";
        if (!nav && !ncx)
            continue;
        const QString base = directoryOf(it->href);
        QXmlStreamReader xml(zip.fileData(it->href));
        QString pending;
        int tocNavDepth = 0;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isEndElement() && nav && xml.name() == u"nav" && tocNavDepth > 0)
                --tocNavDepth;
            if (!xml.isStartElement())
                continue;
            const auto name = xml.name();
            if (nav && name == u"nav") {
                const QString type = xml.attributes().value("http://www.idpf.org/2007/ops", "type").toString()
                                     + xml.attributes().value("epub:type").toString();
                if (type.contains("toc") || tocNavDepth > 0)
                    ++tocNavDepth;
            } else if (nav && name == u"a" && tocNavDepth > 0) {
                const QString href = resolvePath(base, xml.attributes().value("href").toString());
                const QString title = xml.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
                if (!href.isEmpty() && !tocTitles.contains(href))
                    tocTitles.insert(href, title);
            } else if (ncx && name == u"text") {
                pending = xml.readElementText().simplified();
            } else if (ncx && name == u"content") {
                const QString href = resolvePath(base, xml.attributes().value("src").toString());
                if (!href.isEmpty() && !tocTitles.contains(href))
                    tocTitles.insert(href, pending);
            }
        }
    }

    for (const QString &id : spine) {
        const Item item = manifest.value(id);
        if (item.href.isEmpty() || !(item.type.contains("html") || item.type.contains("xml")))
            continue;
        QString documentTitle;
        const QList<Paragraph> paragraphs = htmlParagraphs(zip.fileData(item.href), &documentTitle);
        if (paragraphs.isEmpty())
            continue;
        const QString tocTitle = tocTitles.value(item.href);
        // A spine file the table of contents doesn't list is usually the rest
        // of the previous chapter, split for size.
        if (tocTitle.isEmpty() && !tocTitles.isEmpty() && !book.chapters.isEmpty() && paragraphs.first().level == 0) {
            book.chapters.last().paragraphs.append(paragraphs);
            continue;
        }
        Chapter chapter;
        chapter.paragraphs = paragraphs;
        chapter.title = tocTitle;
        if (chapter.title.isEmpty())
            for (const auto &p : paragraphs)
                if (p.level > 0) {
                    chapter.title = p.text.simplified();
                    break;
                }
        if (chapter.title.isEmpty() && documentTitle != book.title)
            chapter.title = documentTitle;
        if (chapter.title.isEmpty())
            chapter.title = tr("Section %1").arg(book.chapters.size() + 1);
        book.chapters.append(chapter);
    }

    QString coverHref;
    for (auto it = manifest.cbegin(); it != manifest.cend() && coverHref.isEmpty(); ++it)
        if (it->properties.split(' ').contains("cover-image"))
            coverHref = it->href;
    if (coverHref.isEmpty() && manifest.contains(coverId) && manifest.value(coverId).type.startsWith("image/"))
        coverHref = manifest.value(coverId).href;
    if (coverHref.isEmpty())
        for (auto it = manifest.cbegin(); it != manifest.cend() && coverHref.isEmpty(); ++it)
            if (it->type.startsWith("image/") && (it.key().contains("cover", Qt::CaseInsensitive)
                                                  || it->href.contains("cover", Qt::CaseInsensitive)))
                coverHref = it->href;
    if (!coverHref.isEmpty())
        book.cover = zip.fileData(coverHref);
    return true;
}

bool BookLoader::loadFb2(Book &book, const QByteArray &data, QString *error) {
    QXmlStreamReader xml(data);
    QString coverId, firstName, middleName, lastName;
    QHash<QString, QByteArray> binaries;
    QStringList stack;
    int sectionDepth = 0;
    bool inNotes = false;

    auto paragraphText = [&] { return tidy(collectRunText(xml)); };

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement()) {
            if (xml.name() == u"section" && !inNotes)
                --sectionDepth;
            if (xml.name() == u"body")
                inNotes = false;
            if (!stack.isEmpty())
                stack.removeLast();
            continue;
        }
        if (!xml.isStartElement())
            continue;
        const QString name = xml.name().toString();
        const bool inTitleInfo = stack.contains("title-info");
        if (inTitleInfo && name == "book-title") {
            book.title = xml.readElementText().simplified();
            continue;
        }
        if (inTitleInfo && stack.contains("author") && book.author.isEmpty()) {
            if (name == "first-name") { firstName = xml.readElementText().simplified(); continue; }
            if (name == "middle-name") { middleName = xml.readElementText().simplified(); continue; }
            if (name == "last-name") { lastName = xml.readElementText().simplified(); continue; }
        }
        if (inTitleInfo && name == "image" && stack.contains("coverpage")) {
            for (const auto &attribute : xml.attributes())
                if (attribute.name() == u"href")
                    coverId = attribute.value().toString().remove('#');
        }
        if (name == "binary") {
            const QString id = xml.attributes().value("id").toString();
            binaries.insert(id, QByteArray::fromBase64(xml.readElementText().toLatin1()));
            continue;
        }
        if (name == "body") {
            inNotes = xml.attributes().value("name") == u"notes";
        }
        if (inNotes) {
            stack.append(name);
            continue;
        }
        if (name == "section") {
            ++sectionDepth;
        } else if (name == "title" && stack.contains("body")) {
            // A titled top-level section starts a chapter; deeper ones become
            // headings inside it.
            QStringList parts;
            int depth = 1;
            while (depth > 0 && !xml.atEnd()) {
                xml.readNext();
                if (xml.isStartElement() && xml.name() == u"p") {
                    parts.append(paragraphText());
                } else if (xml.isStartElement()) {
                    ++depth;
                } else if (xml.isEndElement()) {
                    --depth;
                }
            }
            const QString title = parts.join(". ").simplified();
            if (title.isEmpty())
                continue;
            if (sectionDepth <= 1 || book.chapters.isEmpty()) {
                book.chapters.append(Chapter{title, {}});
            }
            book.chapters.last().paragraphs.append({title, qBound(1, sectionDepth + 1, 6)});
            continue;
        } else if ((name == "p" || name == "v" || name == "subtitle" || name == "text-author")
                   && stack.contains("body")) {
            const QString text = paragraphText();
            if (book.chapters.isEmpty())
                book.chapters.append(Chapter{book.title, {}});
            if (hasReadableText(text))
                book.chapters.last().paragraphs.append({text, name == "subtitle" ? 4 : 0});
            continue;
        }
        stack.append(name);
    }
    if (xml.hasError() && book.chapters.isEmpty()) {
        *error = tr("This FB2 file is not valid XML: %1").arg(xml.errorString());
        return false;
    }
    book.author = QStringList{firstName, middleName, lastName}.join(' ').simplified();
    if (!coverId.isEmpty())
        book.cover = binaries.value(coverId);
    return true;
}

bool BookLoader::loadDocx(Book &book, QString *error) {
    QZipReader zip(book.path);
    const QByteArray document = zip.fileData("word/document.xml");
    if (!zip.isReadable() || document.isEmpty()) {
        *error = tr("This DOCX has no word/document.xml.");
        return false;
    }
    {
        QXmlStreamReader core(zip.fileData("docProps/core.xml"));
        while (!core.atEnd())
            if (core.readNextStartElement()) {
                if (core.name() == u"title")
                    book.title = core.readElementText().simplified();
                else if (core.name() == u"creator")
                    book.author = core.readElementText().simplified();
            }
    }
    QXmlStreamReader xml(document);
    book.chapters.append(Chapter{});
    while (!xml.atEnd()) {
        if (!xml.readNextStartElement() || xml.name() != u"p")
            continue;
        // Collect the paragraph's runs, noting a heading style on the way.
        int level = 0;
        QString text;
        int depth = 1;
        while (depth > 0 && !xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                ++depth;
                const auto name = xml.name();
                if (name == u"pStyle") {
                    const QString style = xml.attributes().value("w:val").toString();
                    static const QRegularExpression headingStyle(R"(^(?:Heading|Titre|Titulo|Título|berschrift)\s*(\d))",
                                                                 QRegularExpression::CaseInsensitiveOption);
                    const auto match = headingStyle.match(style);
                    level = match.hasMatch() ? match.captured(1).toInt() : style.compare("Title", Qt::CaseInsensitive) == 0 ? 1 : 0;
                } else if (name == u"t") {
                    text += xml.readElementText();
                    --depth;
                } else if (name == u"tab") {
                    text += ' ';
                } else if (name == u"br" || name == u"cr") {
                    text += '\n';
                } else if (name == u"delText" || name == u"instrText" || name == u"footnoteReference") {
                    xml.skipCurrentElement();
                    --depth;
                }
            } else if (xml.isEndElement()) {
                --depth;
            }
        }
        text = tidy(text);
        if (!hasReadableText(text))
            continue;
        if (level == 1 && !book.chapters.last().paragraphs.isEmpty())
            book.chapters.append(Chapter{});
        if (level == 1 && book.chapters.last().title.isEmpty())
            book.chapters.last().title = text;
        book.chapters.last().paragraphs.append({text, level});
    }
    return true;
}

bool BookLoader::loadOdt(Book &book, QString *error) {
    QZipReader zip(book.path);
    const QByteArray content = zip.fileData("content.xml");
    if (!zip.isReadable() || content.isEmpty()) {
        *error = tr("This ODT has no content.xml.");
        return false;
    }
    {
        QXmlStreamReader meta(zip.fileData("meta.xml"));
        while (!meta.atEnd())
            if (meta.readNextStartElement()) {
                if (meta.name() == u"title")
                    book.title = meta.readElementText().simplified();
                else if ((meta.name() == u"creator" || meta.name() == u"initial-creator") && book.author.isEmpty())
                    book.author = meta.readElementText().simplified();
            }
    }
    QXmlStreamReader xml(content);
    book.chapters.append(Chapter{});
    while (!xml.atEnd()) {
        if (!xml.readNextStartElement())
            continue;
        const auto name = xml.name();
        if (name != u"p" && name != u"h")
            continue;
        const int level = name == u"h" ? qMax(1, xml.attributes().value("text:outline-level").toInt()) : 0;
        const QString text = tidy(collectRunText(xml));
        if (!hasReadableText(text))
            continue;
        if (level == 1 && !book.chapters.last().paragraphs.isEmpty())
            book.chapters.append(Chapter{});
        if (level == 1 && book.chapters.last().title.isEmpty())
            book.chapters.last().title = text;
        book.chapters.last().paragraphs.append({text, level});
    }
    return true;
}

bool BookLoader::loadPdf(Book &book, QString *error) {
    const QString pdftotext = QStandardPaths::findExecutable("pdftotext");
    if (pdftotext.isEmpty()) {
        *error = tr("Opening PDF files needs pdftotext. Install it with: omarchy-pkg-add poppler");
        return false;
    }
    const QString pdfinfo = QStandardPaths::findExecutable("pdfinfo");
    if (!pdfinfo.isEmpty()) {
        QProcess info;
        info.start(pdfinfo, {"-enc", "UTF-8", book.path});
        if (info.waitForFinished(15000)) {
            for (const QString &line : QString::fromUtf8(info.readAllStandardOutput()).split('\n')) {
                if (line.startsWith("Title:"))
                    book.title = line.mid(6).simplified();
                else if (line.startsWith("Author:"))
                    book.author = line.mid(7).simplified();
            }
        }
    }
    QProcess process;
    process.start(pdftotext, {"-enc", "UTF-8", "-eol", "unix", book.path, "-"});
    if (!process.waitForFinished(180000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString message = QString::fromUtf8(process.readAllStandardError()).trimmed();
        process.kill();
        *error = message.isEmpty() ? tr("pdftotext could not read this PDF.")
                                   : tr("pdftotext could not read this PDF: %1").arg(message);
        return false;
    }
    book.chapters = pdfChapters(QString::fromUtf8(process.readAllStandardOutput()));
    if (book.chapters.isEmpty()) {
        *error = tr("This PDF has no text layer (it is probably scanned images), so there is nothing to read aloud.");
        return false;
    }
    return true;
}

bool BookLoader::loadConverted(Book &book, QString *error) {
    const QString converter = QStandardPaths::findExecutable("ebook-convert");
    const QString suffix = QFileInfo(book.path).suffix().toUpper();
    if (converter.isEmpty()) {
        *error = tr("Opening %1 files needs Calibre's ebook-convert. Install it with: omarchy-pkg-add calibre").arg(suffix);
        return false;
    }
    // Converted copies live in the cache, keyed by the file's path, size and
    // modification time, so a book converts once and the original is never touched.
    const QFileInfo source(book.path);
    const QByteArray key = QCryptographicHash::hash(
        (source.absoluteFilePath() + QString::number(source.size()) + QString::number(source.lastModified().toSecsSinceEpoch())).toUtf8(),
        QCryptographicHash::Sha1).toHex();
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/converted";
    QDir().mkpath(directory);
    const QString target = directory + "/" + key + ".epub";
    if (!QFileInfo::exists(target)) {
        const QString partial = directory + "/" + key + ".partial.epub";
        QProcess process;
        process.start(converter, {book.path, partial});
        if (!process.waitForFinished(600000) || process.exitCode() != 0) {
            process.kill();
            QFile::remove(partial);
            *error = tr("Calibre could not convert this %1 file (it may be DRM-protected).").arg(suffix);
            return false;
        }
        QFile::rename(partial, target);
    }
    const QString original = book.path;
    book.path = target;
    const bool ok = loadEpub(book, error);
    book.path = original;
    return ok;
}

std::shared_ptr<Book> BookLoader::load(const QString &path, QString *error) {
    QString ignored;
    if (!error)
        error = &ignored;
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        *error = tr("The file %1 does not exist.").arg(path);
        return nullptr;
    }
    if (!info.isReadable()) {
        *error = tr("The file %1 cannot be read.").arg(path);
        return nullptr;
    }

    auto book = std::make_shared<Book>();
    book->path = info.absoluteFilePath();
    QString suffix = info.suffix().toLower();

    // Sniff zip-based EPUBs that arrive with an unexpected extension.
    QFile file(book->path);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray head = file.peek(64);
        if (head.startsWith("PK") && head.contains("application/epub+zip"))
            suffix = "epub";
        else if (head.startsWith("%PDF"))
            suffix = "pdf";
    }
    book->format = suffix;

    bool ok = false;
    if (suffix == "epub") {
        ok = loadEpub(*book, error);
    } else if (suffix == "pdf") {
        ok = loadPdf(*book, error);
    } else if (suffix == "docx") {
        ok = loadDocx(*book, error);
    } else if (suffix == "odt") {
        ok = loadOdt(*book, error);
    } else if (suffix == "fb2" || suffix == "html" || suffix == "htm" || suffix == "xhtml" || suffix == "md"
               || suffix == "markdown" || suffix == "txt" || suffix == "text" || suffix.isEmpty()) {
        if (info.size() > 256 * 1024 * 1024) {
            *error = tr("This file is too large to open as text.");
            return nullptr;
        }
        const QByteArray data = file.readAll();
        if (suffix == "fb2") {
            ok = loadFb2(*book, data, error);
        } else if (suffix == "md" || suffix == "markdown") {
            book->chapters = markdownChapters(decodeText(data));
            ok = true;
        } else if (suffix.startsWith("htm") || suffix == "xhtml") {
            QString title;
            Chapter chapter;
            chapter.paragraphs = htmlParagraphs(data, &title);
            book->title = title;
            book->chapters.append(chapter);
            ok = true;
        } else {
            const QString text = decodeText(data);
            static const QRegularExpression titleLine(R"(^Title:\s*(.+)$)", QRegularExpression::MultilineOption);
            static const QRegularExpression authorLine(R"(^Author:\s*(.+)$)", QRegularExpression::MultilineOption);
            const QString head = text.left(4000);
            book->title = titleLine.match(head).captured(1).trimmed();
            book->author = authorLine.match(head).captured(1).trimmed();
            book->chapters = textChapters(text);
            ok = true;
        }
    } else if (convertedExtensions().contains(suffix)) {
        ok = loadConverted(*book, error);
    } else {
        *error = tr("omareader can't open .%1 files.").arg(suffix);
        return nullptr;
    }
    if (!ok)
        return nullptr;

    dropEmptyChapters(book->chapters);
    if (book->chapters.isEmpty()) {
        *error = tr("No readable text was found in this book.");
        return nullptr;
    }
    if (book->title.isEmpty())
        book->title = info.completeBaseName();
    for (int i = 0; i < book->chapters.size(); ++i)
        if (book->chapters[i].title.isEmpty())
            book->chapters[i].title = i == 0 ? book->title : tr("Section %1").arg(i + 1);
    book->segment();
    return book;
}
