// Made-up books in every format omareader parses natively, built in temp
// directories by the tests. Kept out of the test class's file because moc's
// lexer trips over the XML raw strings.
#include "fixtures.h"

#include <QFile>
#include <private/qzipwriter_p.h>

namespace {
void makeZip(const QString &path, const QList<std::pair<QString, QByteArray>> &files) {
    QZipWriter zip(path);
    for (const auto &[name, data] : files)
        zip.addFile(name, data);
    zip.close();
}
}

namespace Fixtures {

void writeEpub(const QString &path, const QByteArray &png) {
    makeZip(path, {
            {"mimetype", "application/epub+zip"},
            {"META-INF/container.xml", R"(<?xml version="1.0"?><container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"},
            {"OEBPS/content.opf", R"(<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>A Made-up Book</dc:title><dc:creator>Ada Example</dc:creator></metadata>
<manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="c1" href="text/one.xhtml" media-type="application/xhtml+xml"/><item id="c1b" href="text/one-b.xhtml" media-type="application/xhtml+xml"/><item id="c2" href="text/two%20b.xhtml" media-type="application/xhtml+xml"/><item id="img" href="images/cover.png" media-type="image/png" properties="cover-image"/></manifest>
<spine><itemref idref="c1"/><itemref idref="c1b"/><itemref idref="c2"/></spine></package>)"},
            {"OEBPS/nav.xhtml", R"(<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="toc"><ol><li><a href="text/one.xhtml">The Beginning</a></li><li><a href="text/two%20b.xhtml#x">The End</a></li></ol></nav></body></html>)"},
            {"OEBPS/text/one.xhtml", "<html><body><h1>One</h1><p>Once upon a time.</p></body></html>"},
            {"OEBPS/text/one-b.xhtml", "<html><body><p>The middle, split into its own file.</p></body></html>"},
            {"OEBPS/text/two b.xhtml", "<html><body><p>They lived happily.</p></body></html>"},
            {"OEBPS/images/cover.png", png},
    });
}

void writeFb2(const QString &path) {
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(R"(<?xml version="1.0" encoding="utf-8"?>
<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0" xmlns:l="http://www.w3.org/1999/xlink">
<description><title-info><author><first-name>Ivan</first-name><last-name>Example</last-name></author><book-title>Short Tale</book-title></title-info></description>
<body><section><title><p>Part One</p></title><p>First <emphasis>words</emphasis> here.</p><section><title><p>Inner</p></title><p>Nested text.</p></section></section>
<section><title><p>Part Two</p></title><p>Last words.</p></section></body>
<body name="notes"><section><p>A note that is not read.</p></section></body></FictionBook>)");
}

void writeDocx(const QString &path) {
    makeZip(path, {
            {"[Content_Types].xml", "<Types/>"},
            {"docProps/core.xml", R"(<cp:coreProperties xmlns:cp="x" xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Doc Title</dc:title><dc:creator>Doc Author</dc:creator></cp:coreProperties>)"},
            {"word/document.xml", R"(<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>
<w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>Opening</w:t></w:r></w:p>
<w:p><w:r><w:t xml:space="preserve">Hello </w:t></w:r><w:r><w:t>world.</w:t></w:r></w:p>
<w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>Closing</w:t></w:r></w:p>
<w:p><w:r><w:t>Goodbye</w:t><w:delText>deleted</w:delText><w:t>.</w:t></w:r></w:p></w:body></w:document>)"},
    });
}

void writeOdt(const QString &path) {
    makeZip(path, {
            {"mimetype", "application/vnd.oasis.opendocument.text"},
            {"meta.xml", R"(<office:document-meta xmlns:office="o" xmlns:dc="http://purl.org/dc/elements/1.1/"><office:meta><dc:title>Odt Title</dc:title></office:meta></office:document-meta>)"},
            {"content.xml", R"(<office:document-content xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0"><office:body><office:text>
<text:h text:outline-level="1">Heading</text:h><text:p>Two<text:s/>words<text:tab/>and<text:line-break/>a break.</text:p></office:text></office:body></office:document-content>)"},
    });
}

}
