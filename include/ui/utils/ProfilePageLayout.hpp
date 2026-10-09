#pragma once

#include <QGridLayout>
#include <QWidget>

namespace ProfilePageLayout {
// One set of live controls follows the active tab. Reparenting also removes each
// widget from its previous layout, preserving signals, selection and object identity.
inline void attach(QWidget* page, QWidget* groupHint, QWidget* groupList,
                   QWidget* memberToolbar, QWidget* table) {
    if (page == nullptr) return;
    auto* layout = qobject_cast<QGridLayout*>(page->layout());
    if (layout == nullptr) return;
    layout->addWidget(groupHint, 0, 0);
    layout->addWidget(groupList, 1, 0);
    layout->addWidget(memberToolbar, 0, 1);
    layout->addWidget(table, 1, 1);
    layout->setColumnStretch(0, 0);
    layout->setColumnStretch(1, 1);
    layout->setRowStretch(1, 1);
    table->show();
}
}
