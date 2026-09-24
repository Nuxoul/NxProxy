#include "include/ui/profile/edit_selector.h"

#include "include/database/DatabaseManager.h"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/database/entities/Group.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSet>
#include <QVBoxLayout>

EditSelector::EditSelector(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(tr("Choose the proxy profiles that belong to this group. Rules can route traffic to this named selector."), this));

    auto* addRow = new QHBoxLayout;
    candidates = new QComboBox(this);
    addButton = new QPushButton(tr("Add member"), this);
    addRow->addWidget(candidates, 1);
    addRow->addWidget(addButton);
    root->addLayout(addRow);

    members = new QListWidget(this);
    members->setMinimumHeight(120);
    root->addWidget(members, 1);

    auto* removeRow = new QHBoxLayout;
    removeRow->addStretch(1);
    removeButton = new QPushButton(tr("Remove selected"), this);
    removeRow->addWidget(removeButton);
    root->addLayout(removeRow);

    auto* form = new QFormLayout;
    defaultMember = new QComboBox(this);
    form->addRow(tr("Default member:"), defaultMember);
    root->addLayout(form);

    connect(addButton, &QPushButton::clicked, this, [this] {
        const int index = candidates->currentIndex();
        if (index < 0) return;
        const int id = candidates->itemData(index).toInt();
        for (int i = 0; i < members->count(); ++i)
            if (members->item(i)->data(Qt::UserRole).toInt() == id) return;
        auto* item = new QListWidgetItem(candidates->currentText(), members);
        item->setData(Qt::UserRole, id);
        refreshCandidates();
        refreshDefaultMember();
    });
    connect(removeButton, &QPushButton::clicked, this, [this] {
        const int row = members->currentRow();
        if (row < 0) return;
        delete members->takeItem(row);
        refreshCandidates();
        refreshDefaultMember();
    });
}

void EditSelector::refreshCandidates() {
    const int previous = candidates->currentData().isValid() ? candidates->currentData().toInt() : -1;
    QSet<int> selected;
    for (int i = 0; i < members->count(); ++i) selected.insert(members->item(i)->data(Qt::UserRole).toInt());

    candidates->clear();
    if (profile == nullptr) return;
    const auto group = Configs::dataManager->groupsRepo->GetGroup(profile->gid);
    if (group == nullptr) return;
    for (const int id : group->Profiles()) {
        if (selected.contains(id)) continue;
        const auto candidate = Configs::dataManager->profilesRepo->GetProfile(id);
        if (candidate == nullptr || candidate->outbound == nullptr) continue;
        if (candidate->type == "selector" || candidate->type == "autoselector" || candidate->type == "chain"
            || candidate->type == "direct" || candidate->type == "extracore") continue;
        if (const auto custom = candidate->Custom(); custom != nullptr
            && (custom->type == Configs::Custom::CustomFullConfig || custom->type == Configs::Custom::CustomXrayFullConfig)) continue;
        candidates->addItem(candidate->outbound->DisplayTypeAndName(), id);
    }
    const int restored = candidates->findData(previous);
    if (restored >= 0) candidates->setCurrentIndex(restored);
    addButton->setEnabled(candidates->count() > 0);
}

void EditSelector::refreshDefaultMember() {
    const int previous = defaultMember->currentData().isValid() ? defaultMember->currentData().toInt() : -1;
    defaultMember->clear();
    for (int i = 0; i < members->count(); ++i)
        defaultMember->addItem(members->item(i)->text(), members->item(i)->data(Qt::UserRole));
    const int restored = defaultMember->findData(previous);
    if (restored >= 0) defaultMember->setCurrentIndex(restored);
    else if (defaultMember->count() > 0) defaultMember->setCurrentIndex(0);
}

void EditSelector::onStart(std::shared_ptr<Configs::Profile> value) {
    profile = std::move(value);
    const auto selector = profile == nullptr ? nullptr : profile->Selector();
    if (selector == nullptr) return;
    for (const int id : selector->members) {
        const auto member = Configs::dataManager->profilesRepo->GetProfile(id);
        if (member == nullptr || member->outbound == nullptr) continue;
        auto* item = new QListWidgetItem(member->outbound->DisplayTypeAndName(), members);
        item->setData(Qt::UserRole, id);
    }
    refreshCandidates();
    refreshDefaultMember();
    const int current = defaultMember->findData(selector->selectedID);
    if (current >= 0) defaultMember->setCurrentIndex(current);
}

bool EditSelector::onEnd() {
    if (profile == nullptr || profile->Selector() == nullptr) return false;
    auto selector = profile->Selector();
    selector->name = get_edit_text_name ? get_edit_text_name() : selector->name;
    selector->members.clear();
    QSet<int> unique;
    for (int i = 0; i < members->count(); ++i) {
        const int id = members->item(i)->data(Qt::UserRole).isValid() ? members->item(i)->data(Qt::UserRole).toInt() : -1;
        const auto member = Configs::dataManager->profilesRepo->GetProfile(id);
        if (id < 0 || member == nullptr || member->gid != profile->gid || id == profile->id || unique.contains(id)) continue;
        unique.insert(id);
        selector->members.append(id);
    }
    if (selector->members.isEmpty()) {
        QMessageBox::warning(this, tr("Selector"), tr("Add at least one valid member profile."));
        return false;
    }
    const int selected = defaultMember->currentData().isValid() ? defaultMember->currentData().toInt() : -1;
    selector->selectedID = selector->members.contains(selected) ? selected : selector->members.first();
    return true;
}
