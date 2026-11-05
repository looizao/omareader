#pragma once

#include <QDir>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

// Titles for text pasted into the library. A title always starts as the
// text's first few words; a coding agent installed on Omarchy can suggest a
// better one, but only when the user turns that on, since the agent sends
// the text to its provider.
namespace PastedTitle {
// The first few words, with whitespace collapsed and the length capped.
QString fromText(const QString &text, int maxWords = 8, int maxLength = 60);
// An agent's reply reduced to a bare title: its first line, without quotes,
// a "Title:" label or Markdown marks. Empty when nothing usable is left.
QString fromAgentReply(const QString &reply, int maxLength = 100);
} // namespace PastedTitle

// Asks a coding agent for a title, without a terminal and without tools.
// Only agents with a headless mode that can't edit files or run commands
// are supported. Omarchy's default agent goes first, then any other that is
// installed. Exposed to QML as `titleAgent`.
class TitleAgent : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
    Q_PROPERTY(QString name READ name NOTIFY availabilityChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    struct Agent {
        QString id;      // as in ~/.config/omarchy/defaults/agent
        QString name;    // for people
        QString program; // the executable found on the search path
        bool isValid() const { return !program.isEmpty(); }
    };

    // The default agent file and search paths can be replaced for tests.
    explicit TitleAgent(const QString &defaultAgentFile = {}, const QStringList &searchPaths = {},
                        QObject *parent = nullptr);
    ~TitleAgent() override;

    static QStringList supportedAgents();
    static Agent find(const QString &defaultAgentFile, const QStringList &searchPaths = {});
    static QStringList arguments(const QString &id, const QString &prompt);
    static QString prompt(const QString &text);

    bool available() const { return m_agent.isValid(); }
    QString name() const { return m_agent.name; }
    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    bool busy() const { return m_process != nullptr; }
    void setTimeout(int milliseconds) { m_timeout.setInterval(milliseconds); }

    // The title to show before, or instead of, an agent's suggestion.
    Q_INVOKABLE static QString fallbackTitle(const QString &text) { return PastedTitle::fromText(text); }
    // Looks for the agent again, in case one was installed or chosen since.
    Q_INVOKABLE void refresh();
    // Asks for a title for the text; answers with suggested() or failed().
    // A request still running is dropped first.
    Q_INVOKABLE void suggest(const QString &text);
    Q_INVOKABLE void cancel();

signals:
    void availabilityChanged();
    void enabledChanged();
    void busyChanged();
    void suggested(const QString &title);
    void failed(const QString &reason);

private:
    void finish(const QString &title, const QString &reason);

    QString m_defaultAgentFile;
    QStringList m_searchPaths;
    Agent m_agent;
    bool m_enabled = false;
    QProcess *m_process = nullptr;
    std::unique_ptr<QTemporaryDir> m_workDirectory;
    QTimer m_timeout;
};
