#pragma once

#include <QWidget>
#include <QListWidget>
#include <QComboBox>
#include <QPushButton>
#include <memory>

#include "include/ui/profile/profile_editor.h"

class EditSelector final : public QWidget, public ProfileEditor {
    Q_OBJECT
public:
    explicit EditSelector(QWidget* parent = nullptr);
    void onStart(std::shared_ptr<Configs::Profile> profile) override;
    bool onEnd() override;

private:
    std::shared_ptr<Configs::Profile> profile;
    QListWidget* members = nullptr;
    QComboBox* candidates = nullptr;
    QComboBox* defaultMember = nullptr;
    QPushButton* addButton = nullptr;
    QPushButton* removeButton = nullptr;

    void refreshCandidates();
    void refreshDefaultMember();
};
