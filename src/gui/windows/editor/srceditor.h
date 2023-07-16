#ifndef SRCEDITOR_H
#define SRCEDITOR_H

#include "common/memory_ownership.h"
#include "machine/machine.h"

#include <QString>
#include <QSyntaxHighlighter>
#include <QTextEdit>

class SrcEditor : public QTextEdit {
    Q_OBJECT
    using Super = QTextEdit;

public:
    explicit SrcEditor(const QString &text, QWidget *parent = nullptr);
    explicit SrcEditor(QWidget *parent = nullptr);
    QString filename();
    QString title();
    bool loadFile(const QString &filename);
    bool saveFile(QString filename = "");
    bool loadByteArray(const QByteArray &content, const QString &filename = "");
    void setCursorToLine(int ln);
    void setFileName(const QString &filename);
    [[nodiscard]] bool isModified() const;
    void setModified(bool val);
    void setSaveAsRequired(bool val);
    [[nodiscard]] bool saveAsRequired() const;

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    ::Box<QSyntaxHighlighter> highlighter {};
    void setup_common();
    QString fname;
    QString tname;
    bool saveAsRequiredFl {};
};

#endif // SRCEDITOR_H