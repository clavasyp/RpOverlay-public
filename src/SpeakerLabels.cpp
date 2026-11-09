#include "SpeakerLabels.h"

QString SpeakerLabels::autoName(int idx) {
    // 0..25 → A..Z; далее A2,B2,… чтобы не упереться (RP-сцена редко > пары спикеров).
    const int letter = idx % 26;
    const int cycle = idx / 26;
    QString s = QString(QChar('A' + letter));
    if (cycle > 0) s += QString::number(cycle + 1);
    return s;
}

QString SpeakerLabels::labelFor(int speaker) {
    auto itCustom = custom_.constFind(speaker);
    if (itCustom != custom_.constEnd() && !itCustom.value().isEmpty())
        return itCustom.value();

    auto it = order_.constFind(speaker);
    int idx;
    if (it == order_.constEnd()) {
        idx = next_++;
        order_.insert(speaker, idx);
    } else {
        idx = it.value();
    }
    return autoName(idx);
}

void SpeakerLabels::rename(int speaker, const QString &name) {
    if (name.isEmpty())
        custom_.remove(speaker);
    else
        custom_.insert(speaker, name);
}

void SpeakerLabels::reset() {
    order_.clear();
    custom_.clear();
    next_ = 0;
}
