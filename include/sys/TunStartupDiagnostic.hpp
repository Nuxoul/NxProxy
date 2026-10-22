#pragma once
#include <QtGlobal>
#include <QString>
#include <QList>
#include <QRegularExpression>
#include <optional>
#include <algorithm>

namespace TunStartup {
inline constexpr int MaxDetailBytes = 3072;
inline constexpr int MaxCauses = 8;
struct Cause {
    int step = 0; // Protocol TunStartupStep; unknown is explicit, never guessed.
    std::optional<quint32> win32Code;
};
struct SafeDetail {
    QString text;
    bool truncated = false;
    bool redacted = false;
};
inline QString TrimUTF8(const QString& text, int bytes, bool& truncated) {
    auto encoded = text.toUtf8();
    if (encoded.size() <= bytes) return text;
    truncated = true;
    int end = bytes;
    while (end > 0 && (static_cast<unsigned char>(encoded[end]) & 0xc0) == 0x80) --end;
    return QString::fromUtf8(encoded.constData(), end);
}
// Only startup error text, never a configuration or normal connection log.
// Defense in depth for old cores and peer-provided diagnostic strings.
inline SafeDetail SanitizeDetail(const QString& input) {
    SafeDetail result;
    result.truncated = input.size() > 16384;
    auto text = input.left(16384);
    for (auto& ch : text) if (ch.category() == QChar::Other_Control) ch = ' ';
    static const QList<QRegularExpression> patterns{
        QRegularExpression(QStringLiteral(R"((?s)\{.*\}|\[\s*\{.*)")),
        QRegularExpression(QStringLiteral(R"((?i)[a-z][a-z0-9+.-]*://[^\s<>"']+)")),
        QRegularExpression(QStringLiteral(R"((?i)(?:password|passwd|token|secret|authorization|uuid|private[_ -]?key|subscription)\s*[:=]\s*(?:"[^"]*"|'[^']*'|[^\s,;)]+))")),
        QRegularExpression(QStringLiteral(R"("[^"\r\n]*"|'[^'\r\n]*')")),
        QRegularExpression(QStringLiteral(R"((?i)\b[a-z]:[\\/][^;|)\r\n]+|\\\\[^;|)\r\n]+|(?:^|\s)/[^;|)\r\n]+)")),
        QRegularExpression(QStringLiteral(R"(\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?::[0-9]+)?\b)")),
        QRegularExpression(QStringLiteral(R"((?i)\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b)")),
        QRegularExpression(QStringLiteral(R"((?i)(?:[0-9a-f]{0,4}:){2,}[0-9a-f:.%]*)")),
        QRegularExpression(QStringLiteral(R"((?i)\b(?:[a-z0-9-]+\.)+[a-z]{2,63}\b)")),
    };
    for (const auto& pattern : patterns) {
        const auto before = text;
        text.replace(pattern, QStringLiteral("<redacted>"));
        result.redacted = result.redacted || before != text;
    }
    result.text = TrimUTF8(text, MaxDetailBytes, result.truncated);
    return result;
}
inline QString SafeBuild(const QString& build) {
    // Build metadata only, no paths, addresses or arbitrary remote text.
    static const QRegularExpression id(QStringLiteral("^[A-Za-z0-9_.+-]{1,80}$"));
    static const QRegularExpression go(QStringLiteral("^go[0-9]+(?:[.][0-9]+)*(?:(?:rc|beta)[0-9]+)?$"));
    static const QRegularExpression vcs(QStringLiteral("^[0-9a-fA-F]{40}$"));
    if (build.size() > 240) return QStringLiteral("unavailable");
    const auto fields = build.split(';');
    if (fields.isEmpty() || fields.size() > 4) return QStringLiteral("unavailable");
    QStringList seen;
    for (const auto& field : fields) {
        const int split = field.indexOf('=');
        const auto key = field.left(split), value = field.mid(split + 1);
        if (split < 1 || seen.contains(key)) return QStringLiteral("unavailable");
        seen.append(key);
        const bool valid = key == "diagnostic" ? seen.size() == 1 && id.match(value).hasMatch()
            : key == "go" ? go.match(value).hasMatch()
            : key == "vcs" ? vcs.match(value).hasMatch()
            : key == "modified" ? value == "true" || value == "false" : false;
        if (!valid) return QStringLiteral("unavailable");
    }
    return seen.first() == "diagnostic" ? build : QStringLiteral("unavailable");
}
struct Diagnostic {
    bool available = false;
    bool tunStage = false;
    bool configureStage = false;
    quint32 createCode = 0;
    quint32 openCode = 0;
    int reason = 0;
    QList<Cause> causes;
    QString errorDetail;
    QString coreBuild;
    bool detailTruncated = false;
    bool detailRedacted = false;
    bool traversalTruncated = false;
    quint32 version = 0;
};
inline bool RecognizedCode(quint32 code) {
    switch (code) {
    case 0: case 5: case 8: case 31: case 80: case 87: case 183: case 1168: case 1450: return true;
    default: return false;
    }
}
}
