#pragma once
#include <QtGlobal>

namespace TunStartup {
// Bounded informational values, not a recovery policy. No free-form core text.
struct Diagnostic {
    bool available = false;
    bool tunStage = false;
    bool configureStage = false;
    quint32 createCode = 0;
    quint32 openCode = 0;
    int reason = 0;
};
inline bool RecognizedCode(quint32 code) {
    switch (code) {
    case 0: case 5: case 8: case 31: case 80: case 87: case 183: case 1168: case 1450: return true;
    default: return false;
    }
}
}
