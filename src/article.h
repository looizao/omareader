#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

// The readable part of a web page: the article's text without the site's
// navigation, menus, adverts, comments and links to other stories, found the
// way Firefox's Reader View finds it (Mozilla's Readability). The page is
// only parsed: no script runs and nothing it links to is loaded.
struct Article {
    QString title;
    QString author; // the byline, when the page names one
    QString site;   // the site's name, or its host
    // Markdown, as pasted text is kept: headings, paragraphs and list items,
    // with bold, italic and code marked, and everything else escaped.
    QString text;
};

namespace ArticleExtractor {
// What the page reads as. The content type is the response's, whose
// charset wins over the page's own <meta charset>. Plain text and Markdown
// are taken whole. The title falls back to the text's first words.
Article extract(const QByteArray &body, const QUrl &url = {}, const QByteArray &contentType = {});
bool isHtml(const QByteArray &contentType);
// Whether extract() can read this content type.
bool isSupported(const QByteArray &contentType);
// The page's characters, from the content type's charset, the page's
// <meta charset>, or UTF-8 with a fallback to Windows-1252.
QString decode(const QByteArray &body, const QByteArray &contentType);
// The page title without the site's name around it: the name given, or a
// part named after the host.
QString cleanTitle(const QString &title, const QString &site, const QString &host = {});
// Text to put in Markdown so it reads back exactly as it is.
QString escapeMarkdown(const QString &text);
} // namespace ArticleExtractor
