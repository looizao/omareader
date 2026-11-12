#include "article.h"

#include "bookloader.h"
#include "titleagent.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <algorithm>
#include <memory>
#include <vector>

// The approach is Mozilla's Readability (the engine of Firefox's Reader
// View), reduced to what reading aloud needs: score the page's paragraphs,
// credit the elements that hold them, take the best of those with its
// related siblings, and clear what is left of the page around the text.

namespace {

// One element or run of text of the parsed page.
struct Node {
    QString tag; // lower case; empty for text
    QString text;
    QHash<QString, QString> attributes;
    Node *parent = nullptr;
    QList<Node *> children;
    bool dropped = false;
    bool candidate = false;
    double score = 0;

    bool isText() const { return tag.isEmpty(); }
    QString attribute(const QString &name) const { return attributes.value(name); }
};

class Document {
public:
    Document() { root = make("#document"); }
    Node *make(const QString &tag) {
        m_nodes.push_back(std::make_unique<Node>());
        m_nodes.back()->tag = tag;
        return m_nodes.back().get();
    }
    Node *root = nullptr;

private:
    std::vector<std::unique_ptr<Node>> m_nodes;
};

// A tag nested deeper than this is taken in at that depth, so a page can't
// run the recursive passes below out of stack.
constexpr int MaxDepth = 200;

const QSet<QString> &voidTags() {
    static const QSet<QString> tags = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
                                       "param", "source", "track", "wbr", "keygen", "frame"};
    return tags;
}

// Elements whose contents are not markup.
const QSet<QString> &rawTextTags() {
    static const QSet<QString> tags = {"script", "style", "textarea", "title", "noscript", "xmp",
                                       "iframe", "noembed", "noframes", "template"};
    return tags;
}

bool isHeading(const QString &tag) {
    return tag.size() == 2 && tag.at(0) == 'h' && tag.at(1) >= '1' && tag.at(1) <= '6';
}

// Elements that start a block of text: everything else runs inline.
bool isBlock(const QString &tag) {
    static const QSet<QString> tags = {
        "address", "article", "aside", "blockquote", "body", "caption", "center", "dd", "details", "dialog", "div",
        "dl", "dt", "fieldset", "figcaption", "figure", "footer", "form", "header", "hgroup", "hr", "html", "li",
        "main", "menu", "nav", "ol", "p", "pre", "section", "summary", "table", "tbody", "td", "tfoot", "th",
        "thead", "tr", "ul"};
    return tags.contains(tag) || isHeading(tag);
}

// Opening one of these ends an open <p>, as in a browser.
bool closesParagraph(const QString &tag) {
    static const QSet<QString> tags = {
        "address", "article", "aside", "blockquote", "center", "details", "dialog", "dir", "div", "dl", "dd", "dt",
        "fieldset", "figcaption", "figure", "footer", "form", "header", "hgroup", "hr", "li", "main", "menu", "nav",
        "ol", "p", "pre", "section", "summary", "table", "ul"};
    return tags.contains(tag) || isHeading(tag);
}

// Where looking for an open element to close stops.
const QSet<QString> &scopeBoundaries() {
    static const QSet<QString> tags = {"html", "table", "td", "th", "caption", "marquee", "object", "applet",
                                       "template", "button"};
    return tags;
}

// The open element named one of names, or -1 when a boundary comes first.
int openIndex(const QList<Node *> &stack, const QSet<QString> &names, const QSet<QString> &boundaries) {
    for (int i = int(stack.size()) - 1; i > 0; --i) {
        const QString &tag = stack.at(i)->tag;
        if (names.contains(tag))
            return i;
        if (boundaries.contains(tag))
            return -1;
    }
    return -1;
}

QString decodeEntities(const QString &text) {
    if (!text.contains('&'))
        return text;
    QString out;
    out.reserve(text.size());
    const int n = int(text.size());
    for (int i = 0; i < n; ++i) {
        const QChar c = text.at(i);
        if (c == '&') {
            const int semicolon = int(text.indexOf(';', i + 1));
            if (semicolon > i + 1 && semicolon - i <= 32) {
                bool known = false;
                const QString decoded = BookLoader::decodeHtmlEntity(text.mid(i + 1, semicolon - i - 1), &known);
                if (known) {
                    out += decoded;
                    i = semicolon;
                    continue;
                }
            }
        }
        out += c;
    }
    return out;
}

// Reads a start tag's attributes from the character after its name. Returns
// the index of its closing '>' and says whether it ended with "/>".
int readAttributes(const QString &s, int i, QHash<QString, QString> &attributes, bool *selfClosing) {
    const int n = int(s.size());
    *selfClosing = false;
    while (i < n) {
        while (i < n && s.at(i).isSpace())
            ++i;
        if (i >= n)
            return n;
        if (s.at(i) == '>')
            return i;
        if (s.at(i) == '/') {
            if (i + 1 < n && s.at(i + 1) == '>') {
                *selfClosing = true;
                return i + 1;
            }
            ++i;
            continue;
        }
        const int nameStart = i;
        while (i < n && !s.at(i).isSpace() && s.at(i) != '=' && s.at(i) != '>' && s.at(i) != '/')
            ++i;
        const QString name = s.mid(nameStart, i - nameStart).toLower();
        while (i < n && s.at(i).isSpace())
            ++i;
        QString value;
        if (i < n && s.at(i) == '=') {
            ++i;
            while (i < n && s.at(i).isSpace())
                ++i;
            if (i < n && (s.at(i) == '"' || s.at(i) == '\'')) {
                const QChar quote = s.at(i);
                const int end = int(s.indexOf(quote, i + 1));
                value = s.mid(i + 1, (end < 0 ? n : end) - i - 1);
                i = end < 0 ? n : end + 1;
            } else {
                const int valueStart = i;
                while (i < n && !s.at(i).isSpace() && s.at(i) != '>')
                    ++i;
                value = s.mid(valueStart, i - valueStart);
            }
        }
        if (!name.isEmpty() && !attributes.contains(name))
            attributes.insert(name, decodeEntities(value));
    }
    return n;
}

void appendText(Node *parent, const QString &text, Document &doc) {
    if (text.isEmpty())
        return;
    if (!parent->children.isEmpty() && parent->children.last()->isText()) {
        parent->children.last()->text += text;
        return;
    }
    Node *node = doc.make({});
    node->text = text;
    node->parent = parent;
    parent->children.append(node);
}

// A forgiving HTML parser: enough of a browser's tree building (implied end
// tags for paragraphs, list items and table cells, void elements, raw text)
// that real pages, however they are written, come out as their authors see
// them in a browser.
void parse(const QString &s, Document &doc) {
    QList<Node *> stack{doc.root};
    const int n = int(s.size());
    int i = 0;
    while (i < n) {
        const QChar c = s.at(i);
        if (c != '<') {
            const int next = int(s.indexOf('<', i));
            const int end = next < 0 ? n : next;
            appendText(stack.last(), decodeEntities(s.mid(i, end - i)), doc);
            i = end;
            continue;
        }
        if (s.mid(i, 4) == "<!--") {
            const int end = int(s.indexOf("-->", i + 4));
            i = end < 0 ? n : end + 3;
            continue;
        }
        const QChar next = i + 1 < n ? s.at(i + 1) : QChar();
        if (next == '!' || next == '?') {
            const int end = int(s.indexOf('>', i));
            i = end < 0 ? n : end + 1;
            continue;
        }
        const bool closing = next == '/';
        const int nameStart = i + (closing ? 2 : 1);
        if (nameStart >= n || !s.at(nameStart).isLetter()) {
            if (closing) { // "</ " and the like are dropped, as browsers do
                const int end = int(s.indexOf('>', i));
                i = end < 0 ? n : end + 1;
            } else {
                appendText(stack.last(), QStringLiteral("<"), doc);
                ++i;
            }
            continue;
        }
        int nameEnd = nameStart;
        while (nameEnd < n && !s.at(nameEnd).isSpace() && s.at(nameEnd) != '>' && s.at(nameEnd) != '/')
            ++nameEnd;
        QString tag = s.mid(nameStart, nameEnd - nameStart).toLower();
        if (tag.contains(':'))
            tag = tag.section(':', -1);

        if (closing) {
            const int end = int(s.indexOf('>', nameEnd));
            i = end < 0 ? n : end + 1;
            if (tag == "br") { // </br> is a line break in every browser
                Node *br = doc.make("br");
                br->parent = stack.last();
                stack.last()->children.append(br);
                continue;
            }
            int index;
            if (scopeBoundaries().contains(tag)) {
                QSet<QString> boundaries = scopeBoundaries();
                boundaries.remove(tag);
                index = openIndex(stack, {tag}, boundaries);
            } else {
                index = openIndex(stack, {tag}, scopeBoundaries());
            }
            if (index > 0)
                stack.resize(index);
            continue;
        }

        QHash<QString, QString> attributes;
        bool selfClosing = false;
        const int end = readAttributes(s, nameEnd, attributes, &selfClosing);
        i = end + 1;

        // The end tags a browser would imply.
        if (closesParagraph(tag)) {
            const int p = openIndex(stack, {"p"}, scopeBoundaries());
            if (p > 0)
                stack.resize(p);
        }
        if (tag == "li" || tag == "dd" || tag == "dt") {
            const QSet<QString> names = tag == "li" ? QSet<QString>{"li"} : QSet<QString>{"dd", "dt"};
            for (int k = int(stack.size()) - 1; k > 0; --k) {
                const QString &open = stack.at(k)->tag;
                if (names.contains(open)) {
                    stack.resize(k);
                    break;
                }
                if (open != "div" && open != "p" && open != "address" && (isBlock(open) || scopeBoundaries().contains(open)))
                    break;
            }
        } else if (isHeading(tag) && isHeading(stack.last()->tag)) {
            stack.removeLast();
        } else if (tag == "tr") {
            const int open = openIndex(stack, {"tr"}, {"table"});
            if (open > 0)
                stack.resize(open);
        } else if (tag == "td" || tag == "th") {
            const int open = openIndex(stack, {"td", "th"}, {"table", "tr"});
            if (open > 0)
                stack.resize(open);
        } else if (tag == "a" || tag == "option") {
            const int open = openIndex(stack, {tag}, scopeBoundaries());
            if (open > 0)
                stack.resize(open);
        }

        Node *node = doc.make(tag);
        node->attributes = attributes;
        node->parent = stack.last();
        stack.last()->children.append(node);

        if (rawTextTags().contains(tag) && !selfClosing) {
            // Everything up to the matching end tag is text, never markup.
            thread_local QHash<QString, QRegularExpression> ends;
            if (!ends.contains(tag))
                ends.insert(tag, QRegularExpression("</" + tag + "[\\s/>]", QRegularExpression::CaseInsensitiveOption));
            const auto match = ends.value(tag).match(s, i);
            const int close = match.hasMatch() ? int(match.capturedStart()) : n;
            if (tag == "title" || tag == "textarea")
                appendText(node, decodeEntities(s.mid(i, close - i)), doc);
            else if (tag == "script" && attributes.value("type").contains("ld+json"))
                appendText(node, s.mid(i, close - i), doc); // structured data, read for the metadata
            const int after = close < n ? int(s.indexOf('>', close)) : -1;
            i = after < 0 ? n : after + 1;
            continue;
        }
        if (!voidTags().contains(tag) && !selfClosing && stack.size() < MaxDepth)
            stack.append(node);
    }
}

// What a passage of text amounts to: its length with whitespace collapsed,
// how much of it is link text, and its commas, which mark real sentences.
struct Measure {
    double length = 0;
    double linkLength = 0;
    int commas = 0;
};

int countCommas(const QString &text) {
    int commas = 0;
    for (const QChar c : text)
        if (c == ',' || c == QChar(0xFF0C) || c == QChar(0x3001) || c == QChar(0x060C))
            ++commas;
    return commas;
}

void measureInto(const Node *node, Measure &m, double linkWeight) {
    if (node->dropped)
        return;
    if (node->isText()) {
        const QString text = node->text.simplified();
        m.length += text.size();
        m.linkLength += text.size() * linkWeight;
        m.commas += countCommas(text);
        return;
    }
    if (node->tag == "a" && linkWeight == 0) {
        // Links within the page (#notes) count for less than links away.
        linkWeight = node->attribute("href").startsWith('#') ? 0.3 : 1;
    }
    for (const Node *child : node->children)
        measureInto(child, m, linkWeight);
}

Measure measure(const Node *node) {
    Measure m;
    measureInto(node, m, 0);
    return m;
}

double linkDensity(const Node *node) {
    const Measure m = measure(node);
    return m.length > 0 ? m.linkLength / m.length : 0;
}

QString innerText(const Node *node) {
    if (node->dropped)
        return {};
    if (node->isText())
        return node->text;
    QString text;
    for (const Node *child : node->children)
        text += innerText(child);
    return text;
}

// Visits node and every element under it that is still on the page.
template<typename F> void forEach(Node *node, F &&f) {
    if (node->dropped || node->isText())
        return;
    f(node);
    if (node->dropped)
        return;
    for (Node *child : std::as_const(node->children))
        forEach(child, f);
}

int countTags(const Node *node, const QSet<QString> &tags) {
    if (node->dropped || node->isText())
        return 0;
    int count = 0;
    for (const Node *child : node->children)
        if (!child->dropped && !child->isText())
            count += (tags.contains(child->tag) ? 1 : 0) + countTags(child, tags);
    return count;
}

bool hasAncestor(const Node *node, const QString &tag) {
    for (const Node *n = node->parent; n; n = n->parent)
        if (n->tag == tag)
            return true;
    return false;
}

QList<Node *> elementChildren(const Node *node) {
    QList<Node *> children;
    for (Node *child : node->children)
        if (!child->dropped && !child->isText())
            children.append(child);
    return children;
}

Node *findFirst(Node *node, const QString &tag) {
    Node *found = nullptr;
    forEach(node, [&](Node *n) {
        if (!found && n->tag == tag)
            found = n;
    });
    return found;
}

const QRegularExpression &pattern(const char *expression) {
    // Per thread, as pages are read on a worker thread.
    thread_local QHash<QByteArray, QRegularExpression> cache;
    auto it = cache.find(expression);
    if (it == cache.end())
        it = cache.insert(expression, QRegularExpression(QString::fromLatin1(expression),
                                                         QRegularExpression::CaseInsensitiveOption));
    return it.value();
}

// Mozilla's patterns for the class names and ids of what is and isn't text.
const char *Unlikely = "-ad-|ai2html|banner|breadcrumbs|combx|comment|community|cover-wrap|disqus|extra|footer|gdpr|"
                       "header|legends|menu|related|remark|replies|rss|shoutbox|sidebar|skyscraper|social|sponsor|"
                       "supplemental|ad-break|agegate|pagination|pager|popup|yom-remote|cookie|consent|newsletter|"
                       "subscribe|signup";
const char *MaybeText = "and|article|body|column|content|main|mathjax|shadow";
const char *Negative = "-ad-|hidden|^hid$| hid$| hid |^hid |banner|combx|comment|com-|contact|footer|gdpr|masthead|"
                       "media|meta|outbrain|promo|related|scroll|share|shoutbox|sidebar|skyscraper|sponsor|shopping|"
                       "tags|widget";
const char *Positive = "article|body|content|entry|hentry|h-entry|main|page|pagination|post|text|blog|story";
const char *Byline = "byline|author|dateline|writtenby|p-author";
// Lists of citations, and the editing links and boxes of links that wikis
// put around the text: nothing to read aloud, whatever else they hold.
const char *NeverText = "\\breflist\\b|\\breferences\\b|mw-editsection|\\bnavbox|\\bcatlinks\\b";

int classWeight(const Node *node) {
    int weight = 0;
    for (const QString &name : {node->attribute("class"), node->attribute("id")}) {
        if (name.isEmpty())
            continue;
        if (pattern(Negative).match(name).hasMatch())
            weight -= 25;
        if (pattern(Positive).match(name).hasMatch())
            weight += 25;
    }
    return weight;
}

int tagWeight(const QString &tag) {
    if (tag == "div")
        return 5;
    if (tag == "pre" || tag == "td" || tag == "blockquote")
        return 3;
    if (tag == "address" || tag == "ol" || tag == "ul" || tag == "dl" || tag == "dd" || tag == "dt" || tag == "li"
        || tag == "form")
        return -3;
    if (isHeading(tag) || tag == "th")
        return -5;
    return 0;
}

enum Flag { StripUnlikely = 1, WeightClasses = 2, CleanConditionally = 4 };

struct Metadata {
    QString title;
    QString author;
    QString site;
    QString documentTitle;
    QString jsonTitle;
    QString jsonName;
    QString jsonHeadline;
    QString jsonAuthor;
    QString jsonSite;
};

// How much of b's words a holds, from 0 to 1, as Readability measures it.
double similarity(const QString &a, const QString &b) {
    static const QRegularExpression separator(R"(\W+)", QRegularExpression::UseUnicodePropertiesOption);
    const QStringList wordsA = a.toLower().split(separator, Qt::SkipEmptyParts);
    const QStringList wordsB = b.toLower().split(separator, Qt::SkipEmptyParts);
    if (wordsA.isEmpty() || wordsB.isEmpty())
        return 0;
    QStringList unique;
    for (const QString &word : wordsB)
        if (!wordsA.contains(word))
            unique.append(word);
    return 1 - double(unique.join(' ').size()) / wordsB.join(' ').size();
}

// A JSON-LD author: a name, a person, or a list of either.
QString jsonLdNames(const QJsonValue &value) {
    if (value.isString())
        return value.toString();
    if (value.isObject())
        return value.toObject().value("name").toString();
    QStringList names;
    for (const QJsonValue &item : value.toArray()) {
        const QString name = jsonLdNames(item).simplified();
        if (!name.isEmpty() && !names.contains(name))
            names.append(name);
    }
    return names.join(", ");
}

// The article a page describes in its JSON-LD (schema.org), which news
// sites and blogs write for search engines: its headline, author and
// publisher, without the decoration a page title carries.
void readJsonLd(const QString &json, Metadata &meta) {
    QList<QJsonValue> pending{QJsonDocument::fromJson(json.toUtf8()).isArray()
                                  ? QJsonValue(QJsonDocument::fromJson(json.toUtf8()).array())
                                  : QJsonValue(QJsonDocument::fromJson(json.toUtf8()).object())};
    static const QRegularExpression articleType(
        "^(Article|AdvertiserContentArticle|NewsArticle|AnalysisNewsArticle|AskPublicNewsArticle|"
        "BackgroundNewsArticle|OpinionNewsArticle|ReportageNewsArticle|ReviewNewsArticle|Report|SatiricalArticle|"
        "ScholarlyArticle|MedicalScholarlyArticle|SocialMediaPosting|BlogPosting|LiveBlogPosting|"
        "DiscussionForumPosting|TechArticle|APIReference)$");
    for (int guard = 0; !pending.isEmpty() && guard < 200; ++guard) {
        const QJsonValue value = pending.takeFirst();
        if (value.isArray()) {
            for (const QJsonValue &item : value.toArray())
                pending.append(item);
            continue;
        }
        const QJsonObject o = value.toObject();
        if (o.contains("@graph"))
            pending.append(o.value("@graph"));
        QStringList types;
        const QJsonValue type = o.value("@type");
        if (type.isArray())
            for (const QJsonValue &t : type.toArray())
                types.append(t.toString());
        else
            types.append(type.toString());
        bool isArticle = false;
        for (const QString &t : std::as_const(types))
            isArticle = isArticle || articleType.match(t).hasMatch();
        if (!isArticle)
            continue;
        if (meta.jsonTitle.isEmpty()) {
            meta.jsonName = o.value("name").toString().simplified();
            meta.jsonHeadline = o.value("headline").toString().simplified();
            meta.jsonTitle = meta.jsonName.isEmpty() ? meta.jsonHeadline : meta.jsonName;
        }
        if (meta.jsonAuthor.isEmpty())
            meta.jsonAuthor = jsonLdNames(o.value("author")).simplified();
        if (meta.jsonSite.isEmpty())
            meta.jsonSite = o.value("publisher").toObject().value("name").toString().simplified();
        return;
    }
}

Metadata readMetadata(Node *root) {
    Metadata meta;
    QHash<QString, QString> values;
    forEach(root, [&](Node *n) {
        if (n->tag == "meta") {
            const QString content = n->attribute("content").simplified();
            for (const QString &key : {n->attribute("property"), n->attribute("name"), n->attribute("itemprop")})
                if (!key.isEmpty() && !content.isEmpty() && !values.contains(key.toLower()))
                    values.insert(key.toLower(), content);
        } else if (n->tag == "title" && meta.documentTitle.isEmpty()) {
            meta.documentTitle = innerText(n).simplified();
        } else if (n->tag == "script" && meta.jsonTitle.isEmpty()) {
            readJsonLd(innerText(n), meta);
        }
    });
    // Some pages put a summary in the headline (Wikipedia: "species of
    // seabird"), so of the two, the one like the page's title wins.
    if (!meta.jsonName.isEmpty() && !meta.jsonHeadline.isEmpty() && meta.jsonName != meta.jsonHeadline)
        meta.jsonTitle = similarity(meta.jsonHeadline, meta.documentTitle) > 0.75
                                 && similarity(meta.jsonName, meta.documentTitle) <= 0.75
                             ? meta.jsonHeadline
                             : meta.jsonName;
    meta.title = meta.jsonTitle;
    for (const char *key : {"og:title", "twitter:title", "dc.title", "parsely-title"})
        if (meta.title.isEmpty())
            meta.title = values.value(key);
    meta.author = meta.jsonAuthor.contains("://") ? QString() : meta.jsonAuthor;
    for (const char *key : {"author", "article:author", "dc.creator", "parsely-author", "sailthru.author", "byl"}) {
        const QString value = values.value(key);
        if (meta.author.isEmpty() && !value.isEmpty() && !value.contains("://"))
            meta.author = value;
    }
    meta.site = meta.jsonSite;
    for (const char *key : {"og:site_name", "application-name"})
        if (meta.site.isEmpty())
            meta.site = values.value(key);
    return meta;
}

QString cleanByline(QString byline) {
    byline = byline.simplified();
    static const QRegularExpression by(R"(^(by|por|de|par|von)\s+)", QRegularExpression::CaseInsensitiveOption);
    byline.remove(by);
    return byline;
}

bool hidden(const Node *node) {
    if (node->attributes.contains("hidden") || node->attribute("aria-hidden") == "true")
        return true;
    const QString style = node->attribute("style");
    return !style.isEmpty() && pattern(R"(display\s*:\s*none|visibility\s*:\s*hidden)").match(style).hasMatch();
}

// An element's text with its parts kept apart, as a browser shows it.
QString spacedText(const Node *node) {
    if (node->dropped)
        return {};
    if (node->isText())
        return node->text;
    QStringList parts;
    for (const Node *child : node->children)
        parts.append(spacedText(child));
    return parts.join(' ').simplified();
}

bool looksLikeByline(const Node *node) {
    return node->attribute("rel") == "author" || node->attribute("itemprop").contains("author")
           || pattern(Byline).match(node->attribute("class") + ' ' + node->attribute("id")).hasMatch();
}

// The element under node that names the author alone, without the date
// and the buttons a byline box also holds; node itself when none does.
const Node *bylineName(const Node *node) {
    for (const Node *child : node->children) {
        if (child->dropped || child->isText())
            continue;
        const bool name = child->attribute("rel") == "author" || child->attribute("itemprop").contains("author")
                          || pattern("author|writtenby|p-author").match(child->attribute("class") + ' '
                                                                         + child->attribute("id")).hasMatch();
        if (name && !spacedText(child).isEmpty())
            return bylineName(child);
        if (const Node *found = bylineName(child); found != child)
            return found;
    }
    return node;
}

// Clears what is never reading text, and with StripUnlikely what only looks
// like it isn't, and takes the byline out to keep it apart from the text.
void prepare(Node *body, int flags, QString *byline) {
    static const QSet<QString> neverText = {
        "script", "style", "noscript", "template", "svg", "math", "iframe", "object", "embed", "canvas", "video",
        "audio", "button", "input", "select", "textarea", "option", "label", "nav", "aside", "footer", "dialog",
        "figure", "picture", "head", "link", "meta", "sup", "rt", "rp", "menu", "map", "frameset", "frame",
        "applet", "pre", "title"};
    static const QSet<QString> roles = {"menu", "menubar", "complementary", "navigation", "alert", "alertdialog",
                                        "dialog", "banner", "contentinfo", "search"};
    forEach(body, [&](Node *n) {
        if (n != body
            && (neverText.contains(n->tag) || hidden(n) || roles.contains(n->attribute("role").toLower())
                || pattern(NeverText).match(n->attribute("class") + ' ' + n->attribute("id")).hasMatch()))
            n->dropped = true;
    });
    forEach(body, [&](Node *n) {
        if (n == body || !byline->isEmpty() || isHeading(n->tag) || !looksLikeByline(n))
            return;
        const QString text = spacedText(n);
        if (text.isEmpty() || text.size() >= 100)
            return;
        *byline = cleanByline(spacedText(bylineName(n)));
        n->dropped = true;
    });
    if (!(flags & StripUnlikely))
        return;
    forEach(body, [&](Node *n) {
        const QString match = n->attribute("class") + ' ' + n->attribute("id");
        if (n != body && match.size() > 1 && n->tag != "body" && n->tag != "a" && n->tag != "article"
            && n->tag != "main" && pattern(Unlikely).match(match).hasMatch()
            && !pattern(MaybeText).match(match).hasMatch() && !hasAncestor(n, "table") && !hasAncestor(n, "code"))
            n->dropped = true;
    });
}

// The element that holds the article, and its siblings that belong with it.
QList<Node *> grabArticle(Node *body, int flags) {
    QList<Node *> candidates;
    auto credit = [&](Node *holder, const QString &text) {
        const QString simple = text.simplified();
        if (simple.size() < 25)
            return;
        const double points = 1 + countCommas(simple) + std::min(int(simple.size()) / 100, 3);
        int level = 0;
        for (Node *n = holder; n && n->tag != "#document" && level < 5; n = n->parent, ++level) {
            if (!n->candidate) {
                n->candidate = true;
                n->score = tagWeight(n->tag) + ((flags & WeightClasses) ? classWeight(n) : 0);
                candidates.append(n);
            }
            n->score += points / (level == 0 ? 1 : level == 1 ? 2 : level * 3);
        }
    };
    forEach(body, [&](Node *n) {
        if (n->tag == "p" || n->tag == "td") {
            // A cell laid out with paragraphs of its own leaves them to count.
            if (n->tag == "td" && countTags(n, {"p", "div", "table"}) > 0)
                return;
            if (n->parent)
                credit(n->parent, innerText(n));
        } else if (isBlock(n->tag) && !isHeading(n->tag) && n->tag != "li" && n->tag != "dt" && n->tag != "dd"
                   && n->tag != "th" && n->tag != "tr") {
            // Text written straight into a <div>, between its blocks, is a
            // paragraph too, as on pages that break paragraphs with <br>.
            QString direct;
            for (const Node *child : std::as_const(n->children)) {
                if (child->dropped)
                    continue;
                if (child->isText() || !isBlock(child->tag))
                    direct += innerText(child);
                else
                    direct += ' ';
            }
            if (!direct.trimmed().isEmpty())
                credit(n, direct);
        }
    });
    if (candidates.isEmpty())
        return {body};

    // Text buried among links (menus, lists of other stories) counts for less.
    for (Node *n : std::as_const(candidates))
        n->score *= 1 - linkDensity(n);
    std::stable_sort(candidates.begin(), candidates.end(), [](Node *a, Node *b) { return a->score > b->score; });
    Node *top = candidates.first();
    if (top == body || top->tag == "html")
        return {body};

    // Several strong candidates under one element mean the article is split
    // up, and that element holds all of it.
    QList<QList<Node *>> alternatives;
    for (int k = 1; top->score > 0 && k < std::min<qsizetype>(5, candidates.size()); ++k) {
        if (candidates.at(k)->score / top->score < 0.75)
            continue;
        QList<Node *> ancestors;
        for (Node *n = candidates.at(k)->parent; n; n = n->parent)
            ancestors.append(n);
        alternatives.append(ancestors);
    }
    if (alternatives.size() >= 3) {
        for (Node *parent = top->parent; parent && parent != body && parent->tag != "#document";
             parent = parent->parent) {
            int containing = 0;
            for (const auto &ancestors : std::as_const(alternatives))
                containing += ancestors.contains(parent) ? 1 : 0;
            if (containing >= 3) {
                top = parent;
                break;
            }
        }
    }
    // A parent that scores better holds more of the article.
    double last = top->score;
    const double threshold = last / 3;
    for (Node *parent = top->parent; parent && parent != body && parent->tag != "#document"; parent = parent->parent) {
        if (!parent->candidate)
            continue;
        if (parent->score < threshold)
            break;
        if (parent->score > last) {
            top = parent;
            break;
        }
        last = parent->score;
    }
    // A wrapper with nothing else in it is the same thing.
    while (top->parent && top->parent != body && top->parent->tag != "#document"
           && elementChildren(top->parent).size() == 1)
        top = top->parent;
    if (!top->candidate) {
        top->candidate = true;
        top->score = tagWeight(top->tag) + ((flags & WeightClasses) ? classWeight(top) : 0);
    }

    Node *parent = top->parent;
    if (!parent || parent->tag == "#document")
        return {top};
    const double siblingThreshold = std::max(10.0, top->score * 0.2);
    const QString topClass = top->attribute("class");
    QList<Node *> content;
    for (Node *sibling : parent->children) {
        if (sibling->dropped)
            continue;
        bool include = sibling == top;
        if (!include && sibling->isText()) {
            continue;
        } else if (!include) {
            const double bonus = !topClass.isEmpty() && sibling->attribute("class") == topClass ? top->score * 0.2 : 0;
            if (sibling->candidate && sibling->score + bonus >= siblingThreshold) {
                include = true;
            } else if (sibling->tag == "p") {
                const Measure m = measure(sibling);
                const double density = m.length > 0 ? m.linkLength / m.length : 0;
                const QString text = innerText(sibling).simplified();
                include = (m.length > 80 && density < 0.25)
                          || (m.length > 0 && m.length < 80 && density == 0
                              && pattern(R"(\.( |$))").match(text).hasMatch());
            }
        }
        if (include)
            content.append(sibling);
    }
    return content;
}

// Clears what the chosen part of the page still holds that isn't its text:
// boxes of links, galleries, forms and headings of other things.
void clean(Node *node, int flags) {
    for (Node *child : std::as_const(node->children))
        if (!child->dropped && !child->isText())
            clean(child, flags);
    if (node->dropped)
        return;
    const QString &tag = node->tag;
    if ((tag == "h1" || tag == "h2") && (flags & WeightClasses) && classWeight(node) < 0) {
        node->dropped = true;
        return;
    }
    static const QSet<QString> conditional = {"table", "ul", "ol", "div", "section", "form", "fieldset"};
    if (!(flags & CleanConditionally) || !conditional.contains(tag))
        return;
    const int weight = (flags & WeightClasses) ? classWeight(node) : 0;
    const double score = node->candidate ? node->score : 0;
    if (weight + score < 0) {
        node->dropped = true;
        return;
    }
    const Measure m = measure(node);
    if (m.commas >= 10)
        return;
    const double density = m.length > 0 ? m.linkLength / m.length : 0;
    bool isList = tag == "ul" || tag == "ol";
    if (!isList && m.length > 0) {
        double listLength = 0;
        forEach(node, [&](Node *n) {
            if (n != node && (n->tag == "ul" || n->tag == "ol"))
                listLength += measure(n).length;
        });
        isList = listLength / m.length > 0.9;
    }
    const int p = countTags(node, {"p"});
    const int img = countTags(node, {"img"});
    const int li = countTags(node, {"li"}) - 100;
    const int input = countTags(node, {"input", "select", "textarea", "button"});
    double headingLength = 0;
    forEach(node, [&](Node *n) {
        if (isHeading(n->tag))
            headingLength += measure(n).length;
    });
    const double headingDensity = m.length > 0 ? headingLength / m.length : 0;
    const bool inFigure = hasAncestor(node, "figure");
    const bool remove = (img > 1 && double(p) / img < 0.5 && !inFigure) || (!isList && li > p)
                        || (input > p / 3) // forms
                        || (!isList && headingDensity < 0.9 && m.length < 25 && (img == 0 || img > 2) && !inFigure)
                        || (!isList && weight < 25 && density > 0.2) || (weight >= 25 && density > 0.5)
                        // Lists that are mostly links are menus and lists of other stories.
                        || (isList && density > 0.5);
    if (remove)
        node->dropped = true;
}

// A block of the article as it will read: a heading, a paragraph or a list
// item, its text in runs of the same formatting.
struct Run {
    QString text;
    int flags = 0;
};

struct Block {
    int level = 0;
    bool item = false;
    QList<Run> runs;
};

class Renderer {
public:
    QList<Block> blocks;

    void render(const Node *node, int style) {
        if (node->dropped)
            return;
        if (node->isText()) {
            QString text = node->text;
            text.replace(QChar(0x00A0), ' ');
            static const QRegularExpression space(R"(\s+)");
            text.replace(space, QStringLiteral(" "));
            m_current.runs.append({text, style});
            return;
        }
        const QString &tag = node->tag;
        if (tag == "br" || tag == "hr") {
            flush();
            return;
        }
        if (tag == "td" || tag == "th") {
            m_current.runs.append({QStringLiteral(" "), 0});
            children(node, style);
            m_current.runs.append({QStringLiteral(" "), 0});
            return;
        }
        if (isHeading(tag)) {
            flush();
            m_level = tag.at(1).digitValue();
            children(node, style);
            flush();
            m_level = 0;
            return;
        }
        if (tag == "li") {
            flush();
            ++m_items;
            children(node, style);
            flush();
            --m_items;
            return;
        }
        if (isBlock(tag)) {
            flush();
            children(node, style);
            flush();
            return;
        }
        int added = 0;
        if (tag == "b" || tag == "strong")
            added = TextStyle::Bold;
        else if (tag == "i" || tag == "em")
            added = TextStyle::Italic;
        else if (tag == "code" || tag == "kbd" || tag == "samp" || tag == "tt")
            added = TextStyle::Code;
        else if (tag == "s" || tag == "strike" || tag == "del")
            added = TextStyle::Strike;
        children(node, style | added);
    }

    void flush() {
        QString text;
        for (const Run &run : std::as_const(m_current.runs))
            text += run.text;
        bool readable = false;
        for (const QChar c : text)
            if (c.isLetterOrNumber()) {
                readable = true;
                break;
            }
        if (readable) {
            m_current.level = m_level;
            m_current.item = m_items > 0 && m_level == 0;
            blocks.append(m_current);
        }
        m_current = Block{};
    }

private:
    void children(const Node *node, int style) {
        for (const Node *child : node->children)
            render(child, style);
    }

    Block m_current;
    int m_level = 0;
    int m_items = 0;
};

QString escapeInline(const QString &text) {
    QString out;
    out.reserve(text.size() + 8);
    for (const QChar c : text) {
        if (c == '\\' || c == '`' || c == '*' || c == '_' || c == '~' || c == '[')
            out += '\\';
        out += c;
    }
    return out;
}

// Characters that would start a Markdown block at the start of a line.
QString escapeLineStart(const QString &line) {
    static const QRegularExpression numbered(R"(^(\d+)([.)]))");
    const auto match = numbered.match(line);
    if (match.hasMatch())
        return match.captured(1) + '\\' + line.mid(match.capturedLength(1));
    if (line.startsWith('#') || line.startsWith('>') || line.startsWith('+') || line.startsWith('-'))
        return '\\' + line;
    return line;
}

// The block's text with whitespace collapsed, in runs of one formatting.
QList<Run> tidyRuns(const QList<Run> &runs) {
    QList<Run> out;
    QString all;
    for (const Run &run : runs) {
        for (QChar c : run.text) {
            if (c.isSpace()) {
                if (all.isEmpty() || all.endsWith(' '))
                    continue;
                c = ' ';
            }
            all += c;
            // A space takes the formatting of the text before it, so marks
            // never open or close on one.
            const int flags = c == ' ' && !out.isEmpty() ? out.last().flags : run.flags;
            if (!out.isEmpty() && out.last().flags == flags)
                out.last().text += c;
            else
                out.append({QString(c), flags});
        }
    }
    while (!out.isEmpty() && out.last().text.endsWith(' ')) {
        out.last().text.chop(1);
        if (out.last().text.isEmpty())
            out.removeLast();
    }
    return out;
}

QString wrap(const QString &text, int flags, bool escape) {
    const auto escaped = [escape](const QString &t) { return escape ? escapeInline(t) : t; };
    // Runs come with whitespace collapsed, so a space at most on either side.
    const int lead = text.startsWith(' ') ? 1 : 0;
    const int trail = text.endsWith(' ') && text.size() > lead ? 1 : 0;
    const QString core = text.mid(lead, text.size() - lead - trail);
    if (core.isEmpty() || !flags)
        return escaped(text);
    QString marked;
    if (flags & TextStyle::Code) {
        int longest = 0, run = 0;
        for (const QChar c : core) {
            run = c == '`' ? run + 1 : 0;
            longest = std::max(longest, run);
        }
        const QString ticks(longest + 1, '`');
        const QString pad = core.startsWith('`') || core.endsWith('`') ? QStringLiteral(" ") : QString();
        marked = ticks + pad + core + pad + ticks;
    } else {
        marked = escaped(core);
    }
    if (flags & TextStyle::Strike)
        marked = "~~" + marked + "~~";
    if (flags & TextStyle::Italic)
        marked = '*' + marked + '*';
    if (flags & TextStyle::Bold)
        marked = "**" + marked + "**";
    return QString(lead, ' ') + marked + QString(trail, ' ');
}

// The formatting the runs should read back with, as markdownInline gives it.
QList<TextStyle> stylesOf(const QList<Run> &runs) {
    QList<TextStyle> styles;
    int at = 0;
    for (const Run &run : runs) {
        const int lead = run.text.startsWith(' ') ? 1 : 0;
        const int trail = run.text.endsWith(' ') && run.text.size() > lead ? 1 : 0;
        const int length = int(run.text.size()) - lead - trail;
        if (run.flags && length > 0) {
            if (!styles.isEmpty() && styles.last().flags == run.flags
                && styles.last().start + styles.last().length == at + lead)
                styles.last().length += length;
            else
                styles.append({at + lead, length, run.flags});
        }
        at += int(run.text.size());
    }
    return styles;
}

// Whether a line of Markdown reads back as the block it was written for.
bool readsAs(const QString &line, const QString &plain, int level, const QList<TextStyle> &styles) {
    const QList<Chapter> chapters = BookLoader::markdownChapters(line, false);
    if (chapters.size() != 1 || chapters.first().paragraphs.size() != 1)
        return false;
    const Paragraph &p = chapters.first().paragraphs.first();
    return p.text == plain && p.level == level && p.styles == styles;
}

// The block as a line of Markdown, escaped only where Markdown would
// otherwise read it differently, so the text stays easy to review.
QString toMarkdown(const Block &block, int *length) {
    const QList<Run> runs = tidyRuns(block.runs);
    const QList<TextStyle> styles = stylesOf(runs);
    QString plain;
    for (const Run &run : runs)
        plain += run.text;
    *length = int(plain.size());
    const QString prefix = block.level > 0 ? QString(block.level, '#') + ' ' : block.item ? QStringLiteral("- ") : QString();
    for (const bool escape : {false, true}) {
        QString marked;
        for (const Run &run : runs)
            marked += wrap(run.text, run.flags, escape);
        for (const QString &line : {prefix + marked, prefix + escapeLineStart(marked)})
            if (readsAs(line, plain, block.level, styles))
                return line;
    }
    // Formatting Markdown can't express here is dropped rather than shown
    // as stray marks.
    return prefix + escapeLineStart(escapeInline(plain));
}

QString render(const QList<Node *> &content, int *length) {
    Renderer renderer;
    for (const Node *node : content)
        renderer.render(node, 0);
    renderer.flush();
    // A heading whose section was cleared away (a list of links, say) has
    // nothing left to introduce.
    for (int k = int(renderer.blocks.size()) - 1; k >= 0; --k) {
        const int level = renderer.blocks.at(k).level;
        if (level > 0
            && (k + 1 == renderer.blocks.size()
                || (renderer.blocks.at(k + 1).level > 0 && renderer.blocks.at(k + 1).level <= level)))
            renderer.blocks.removeAt(k);
    }
    QStringList lines;
    *length = 0;
    for (const Block &block : std::as_const(renderer.blocks)) {
        int blockLength = 0;
        lines.append(toMarkdown(block, &blockLength));
        *length += blockLength;
    }
    return lines.join("\n\n");
}

QString hostName(const QUrl &url) {
    QString host = url.host();
    if (host.startsWith("www."))
        host = host.mid(4);
    return host;
}

QByteArray mimeType(const QByteArray &contentType) { return contentType.split(';').first().trimmed().toLower(); }

} // namespace

namespace ArticleExtractor {

bool isHtml(const QByteArray &contentType) {
    const QByteArray mime = mimeType(contentType);
    return mime.isEmpty() || mime == "text/html" || mime == "application/xhtml+xml";
}

bool isSupported(const QByteArray &contentType) {
    const QByteArray mime = mimeType(contentType);
    return isHtml(contentType) || mime == "text/plain" || mime == "text/markdown" || mime == "text/x-markdown";
}

QString decode(const QByteArray &body, const QByteArray &contentType) {
    static const QRegularExpression headerCharset(R"(charset\s*=\s*["']?([\w.:-]+))",
                                                  QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression metaCharset(R"(<meta[^>]+charset\s*=\s*["']?([\w.:-]+))",
                                                QRegularExpression::CaseInsensitiveOption);
    if (body.startsWith("\xEF\xBB\xBF") || body.startsWith("\xFF\xFE") || body.startsWith("\xFE\xFF"))
        return BookLoader::decodeText(body);
    QString charset = headerCharset.match(QString::fromLatin1(contentType)).captured(1).toLower();
    if (charset.isEmpty() && isHtml(contentType))
        charset = metaCharset.match(QString::fromLatin1(body.left(4096))).captured(1).toLower();
    // Browsers read pages labelled Latin-1 or ASCII as Windows-1252, and
    // the pages are written for that.
    if (charset == "iso-8859-1" || charset == "latin1" || charset == "us-ascii" || charset == "ascii")
        charset = "windows-1252";
    if (!charset.isEmpty() && charset != "utf-8" && charset != "utf8") {
        QStringDecoder decoder(charset.toLatin1().constData());
        if (decoder.isValid())
            return decoder(body);
    }
    return BookLoader::decodeText(body);
}

QString cleanTitle(const QString &title, const QString &site, const QString &host) {
    const QString simple = title.simplified();
    static const QStringList separators = {" | ", " - ", QString::fromUtf8(" – "), QString::fromUtf8(" — "),
                                           QString::fromUtf8(" · "), " :: ", QString::fromUtf8(" » "), " / "};
    if (!site.isEmpty()) {
        for (const QString &separator : separators) {
            if (simple.endsWith(separator + site, Qt::CaseInsensitive))
                return simple.left(simple.size() - separator.size() - site.size()).trimmed();
            if (simple.startsWith(site + separator, Qt::CaseInsensitive))
                return simple.mid(site.size() + separator.size()).trimmed();
        }
    }
    auto letters = [](const QString &text) {
        QString out;
        for (const QChar c : text)
            if (c.isLetterOrNumber())
                out += c.toLower();
        return out;
    };
    // A part named after the host is the site ("Arctic tern - Wikipedia" on
    // en.wikipedia.org).
    auto namesHost = [&](const QString &part) {
        const QString name = letters(part);
        for (const QString &label : host.split('.'))
            if (label.size() >= 3 && label != "www" && !name.isEmpty() && (name.contains(label) || label.contains(name)))
                return true;
        return false;
    };
    QString current = simple;
    for (int round = 0; round < 3; ++round) {
        int cut = -1, separatorSize = 0;
        for (const QString &separator : separators) {
            const int at = int(current.lastIndexOf(separator));
            if (at > cut) {
                cut = at;
                separatorSize = int(separator.size());
            }
        }
        if (cut <= 0)
            break;
        const QString before = current.left(cut).trimmed();
        // Otherwise the part before the last separator is the title when it
        // is long enough to be one, once.
        if (namesHost(current.mid(cut + separatorSize))
            || (round == 0 && before.split(' ', Qt::SkipEmptyParts).size() >= 3))
            current = before;
        else
            break;
    }
    return current;
}

QString escapeMarkdown(const QString &text) {
    QStringList lines = text.split('\n');
    for (QString &line : lines) {
        int indent = 0;
        while (indent < line.size() && line.at(indent).isSpace())
            ++indent;
        line = line.left(indent) + escapeLineStart(escapeInline(line.mid(indent)));
    }
    return lines.join('\n');
}

Article extract(const QByteArray &body, const QUrl &url, const QByteArray &contentType) {
    Article article;
    const QString host = hostName(url);
    const QByteArray mime = mimeType(contentType);
    const QString path = url.path().toLower();
    const QString decoded = decode(body, contentType);

    if (!isHtml(contentType)) {
        const bool markdown = mime != "text/plain" || path.endsWith(".md") || path.endsWith(".markdown");
        article.text = markdown ? decoded : escapeMarkdown(decoded);
        article.text.replace("\r\n", "\n");
        article.text = article.text.trimmed();
        article.site = host;
        article.title = PastedTitle::fromText(article.text);
        return article;
    }

    // Each try starts from a fresh parse, since the last one dropped parts
    // of the page. Readability's order: first keep what only looks unlikely,
    // then stop trusting class names, then stop clearing boxes.
    const int attempts[] = {StripUnlikely | WeightClasses | CleanConditionally, WeightClasses | CleanConditionally,
                            CleanConditionally, 0};
    QString best;
    int bestLength = -1;
    Metadata meta;
    QString byline;
    for (const int flags : attempts) {
        Document doc;
        parse(decoded, doc);
        if (bestLength < 0)
            meta = readMetadata(doc.root);
        if (meta.title.isEmpty() && meta.documentTitle.isEmpty()) {
            if (Node *h1 = findFirst(doc.root, "h1"))
                meta.documentTitle = innerText(h1).simplified();
        }
        Node *body = findFirst(doc.root, "body");
        if (!body)
            body = doc.root;
        QString foundByline;
        prepare(body, flags, &foundByline);
        if (byline.isEmpty())
            byline = foundByline;
        QList<Node *> content = grabArticle(body, flags);
        for (Node *node : std::as_const(content))
            clean(node, flags);
        int length = 0;
        const QString text = render(content, &length);
        if (length > bestLength) {
            best = text;
            bestLength = length;
        }
        if (length >= 500)
            break;
    }

    article.text = best;
    article.site = meta.site.isEmpty() ? host : meta.site.simplified();
    article.author = meta.author.isEmpty() ? byline : cleanByline(meta.author);
    article.title = cleanTitle(meta.title.isEmpty() ? meta.documentTitle : meta.title, article.site, host);
    // The article's own heading repeats the title, which the reader already
    // shows and reads out at the start.
    if (article.text.startsWith('#') && !article.title.isEmpty()) {
        const qsizetype end = article.text.indexOf("\n\n");
        const QString first = end < 0 ? article.text : article.text.left(end);
        const QList<Chapter> heading = BookLoader::markdownChapters(first, false);
        if (!heading.isEmpty() && heading.first().paragraphs.size() == 1
            && heading.first().paragraphs.first().level > 0
            && similarity(heading.first().paragraphs.first().text, article.title) > 0.75)
            article.text = end < 0 ? QString() : article.text.mid(end + 2);
    }
    if (article.title.isEmpty())
        article.title = PastedTitle::fromText(article.text);
    return article;
}

} // namespace ArticleExtractor
